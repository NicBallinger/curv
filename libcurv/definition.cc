// Copyright 2016-2020 Doug Moen
// Licensed under the Apache License, version 2.0
// See accompanying file LICENSE or https://www.apache.org/licenses/LICENSE-2.0

#include <algorithm>
#include <functional>
#include <iostream>

#include <libcurv/context.h>
#include <libcurv/definition.h>
#include <libcurv/die.h>
#include <libcurv/exception.h>
#include <libcurv/system.h>

namespace curv
{

void
Data_Definition::add_to_scope(Recursive_Scope& scope)
{
    unsigned unitnum = scope.add_unit(share(*this));
    pattern_ = make_pattern(*definiendum_, scope, unitnum);
}
void
Function_Definition::add_to_scope(Recursive_Scope& scope)
{
    unsigned unitnum = scope.add_unit(share(*this));
    auto b = scope.add_binding(name_->symbol_, *syntax_, unitnum);
    slot_ = b.first;
}
void
Data_Definition::analyse(Environ& env)
{
    pattern_->analyse(env);
    definiens_expr_ = analyse_op(*definiens_phrase_, env);
}
void
Function_Definition::analyse(Environ& env)
{
    lambda_phrase_->shared_nonlocals_ = true;
    auto expr = analyse_op(*lambda_phrase_, env);
    auto lambda = cast<Lambda_Expr>(expr);
    assert(lambda != nullptr);
    int argpos = 0;
    auto lam = lambda;
    while (lam != nullptr) {
        lam->name_ = name_->symbol_;
        lam->argpos_ = argpos++;
        lam = cast<Lambda_Expr>(lam->body_);
    }
    lambda_ = make<Lambda>(lambda->pattern_, lambda->body_, lambda->nslots_);
    lambda_->name_ = name_->symbol_;
}
Shared<Operation>
Data_Definition::make_setter(slot_t module_slot)
{
    return make<Data_Setter>(
        syntax_, module_slot, pattern_, definiens_expr_);
}
Shared<Operation>
Function_Definition::make_setter(slot_t module_slot)
{
    die("Function_Definition::make_setter: must not be called");
}

void
Include_Definition::add_to_scope(Recursive_Scope& scope)
{
    // Evaluate the argument to `include` in the builtin environment.
    auto val = std_eval(*arg_, scope);
    At_Phrase cx(*arg_, scope);
    auto record = val.to<Record>(cx);

    // construct an Include_Setter from the record argument.
    unsigned unit = scope.add_unit(share(*this));
    setter_ = {Include_Setter::make(record->size(), syntax_)};
    size_t i = 0;
    record->each_field(cx, [&](Symbol_Ref name, Value value)->void {
        auto b = scope.add_binding(name, *syntax_, unit);
        (*setter_)[i++] = {b.first, value};
    });
}
void
Include_Definition::analyse(Environ&)
{
}
Shared<Operation>
Include_Definition::make_setter(slot_t module_slot)
{
    setter_->module_slot_ = module_slot;
    return setter_;
}

void
Test_Definition::add_to_scope(Recursive_Scope& scope)
{
    unsigned unitnum = scope.add_unit(share(*this));
    (void) unitnum;
}
void
Test_Definition::analyse(Environ& env)
{
    setter_ = analyse_op(*arg_, env);
}
Shared<Operation>
Test_Definition::make_setter(slot_t module_slot)
{
    return setter_;
}

void
Compound_Definition_Base::add_to_scope(Recursive_Scope& scope)
{
    for (auto &def : *this) {
        def->add_to_scope(scope);
    }
}

std::pair<slot_t, Shared<const Scoped_Variable>>
Scope::add_binding(Symbol_Ref name, const Phrase& unitsrc, unsigned unitno)
{
    if (dictionary_.find(name) != dictionary_.end())
        throw Exception(At_Phrase(unitsrc, *parent_),
            stringify(name, ": multiply defined"));
    slot_t slot = make_slot();
    auto var = make<Scoped_Variable>();
    dictionary_.emplace(std::make_pair(name, Binding{slot, unitno, var}));
    return std::make_pair(slot,var);
}
Shared<Meaning>
Scope::single_lookup(const Identifier& id)
{
    auto b = dictionary_.find(id.symbol_);
    if (b != dictionary_.end())
        return make<Local_Data_Ref>(share(id), b->second.slot_index_,
            b->second.variable_);
    return nullptr;
}
Shared<Locative>
Scope::single_lvar_lookup(const Identifier& id)
{
    auto b = dictionary_.find(id.symbol_);
    if (b != dictionary_.end()) {
        b->second.variable_->is_mutable_ = true;
        return make<Local_Locative>(share(id), b->second.slot_index_);
    }
    return nullptr;
}

std::pair<slot_t, Shared<const Scoped_Variable>>
Recursive_Scope::add_binding(
    Symbol_Ref name, const Phrase& unitsrc, unsigned unitno)
{
    if (dictionary_.find(name) != dictionary_.end())
        throw Exception(At_Phrase(unitsrc, *parent_),
            stringify(name, ": multiply defined"));
    slot_t slot = (target_is_module_ ? dictionary_.size() : make_slot());
    auto var = make<Scoped_Variable>();
    dictionary_.emplace(std::make_pair(name, Binding{slot, unitno, var}));
    return std::make_pair(slot,var);
}

void
Recursive_Scope::analyse(Definition& def)
{
    def.add_to_scope(*this);
    analyse();
}
void
Recursive_Scope::analyse()
{
    for (auto a : action_phrases_) {
        if (isa<const Local_Phrase>(a)) {
            throw Exception(At_Phrase(*a, *this),
                "local definitions can't be mixed with recursive definitions");
        }
        auto op = analyse_op(*a, *this);
        executable_.actions_.push_back(op);
    }
    for (auto& unit : units_) {
        if (unit.state_ == Unit::k_not_analysed)
            analyse_unit(unit, nullptr);
    }
    parent_->frame_maxslots_ = frame_maxslots_;
    if (target_is_module_) {
        auto d = make<Module::Dictionary>();
        for (auto b : dictionary_)
            (*d)[b.first] = b.second.slot_index_;
        executable_.module_dictionary_ = d;
    }
}

// Analyse the unitary definition `unit` that belongs to the scope,
// then output an action that initializes its bindings to `executable_`.
// As a side effect of analysing `unit`, all of the units it depends on will
// first be analysed, and their initialization actions will first be output.
// This ordering means that slots are initialized in dependency order.
//
// Use Tarjan's algorithm for Strongly Connected Components (SCC)
// to group mutually recursive function definitions together into a single
// initialization action.
void
Recursive_Scope::analyse_unit(Unit& unit, const Identifier* id)
{
    switch (unit.state_) {
    case Unit::k_not_analysed:
        unit.state_ = Unit::k_analysis_in_progress;
        unit.scc_ord_ = unit.scc_lowlink_ = scc_count_++;
        scc_stack_.push_back(&unit);

        analysis_stack_.push_back(&unit);
        if (unit.is_function()) {
            Function_Environ fenv(*this, unit);
            unit.def_->analyse(fenv);
            frame_maxslots_ = std::max(frame_maxslots_, fenv.frame_maxslots_);
        } else {
            unit.def_->analyse(*this);
        }
        analysis_stack_.pop_back();

        if (!analysis_stack_.empty()) {
            Unit* parent = analysis_stack_.back();
            if (unit.scc_lowlink_ < parent->scc_lowlink_) {
                parent->scc_lowlink_ = unit.scc_lowlink_;
                if (!unit.is_function()) {
                    throw Exception(At_Phrase(*id, *this),
                        "illegal recursive reference");
                }
            }
        }
        break;
    case Unit::k_analysis_in_progress:
      {
        // Recursion detected. Unit is already on the SCC and analysis stacks.
        if (!unit.is_function()) {
            throw Exception(At_Phrase(*id, *this),
                "illegal recursive reference");
        }
        assert(!analysis_stack_.empty());
        Unit* parent = analysis_stack_.back();
        parent->scc_lowlink_ = std::min(parent->scc_lowlink_, unit.scc_ord_);
        // For example, the analysis stack might contain 0->1->2, and now we
        // are back to 0, ie unit.scc_ord_==0 (recursion detected).
        // In the above statement, we are propagating lowlink=0 to unit 2.
        // In the k_not_analysed case above, once we pop the analysis stack,
        // we'll further propagate 2's lowlink of 0 to unit 1.
        return;
      }
    case Unit::k_analysed:
        return;
    }
    assert(unit.state_ == Unit::k_analysis_in_progress);
    if (unit.scc_lowlink_ == unit.scc_ord_) {
        // `unit` is the lowest unit in its SCC. All members of this SCC
        // are on the SCC stack. Output an initialization action for unit's SCC.
        if (!unit.is_function()) {
            assert(scc_stack_.back() == &unit);
            scc_stack_.pop_back();
            unit.state_ = Unit::k_analysed;
            executable_.actions_.push_back(
                unit.def_->make_setter(executable_.module_slot_));
        } else {
            // Output a Function_Setter to initialize the slots for a group of
            // mutually recursive functions, or a single nonrecursive function.

            size_t ui = 0;
            while (ui < scc_stack_.size() && scc_stack_[ui] != &unit)
                ++ui;
            assert(scc_stack_[ui] == &unit);

            executable_.actions_.push_back(
                make_function_setter(scc_stack_.size()-ui, &scc_stack_[ui]));
            Unit* u;
            do {
                assert(scc_stack_.size() > 0);
                u = scc_stack_.back();
                scc_stack_.pop_back();
                assert(u->scc_lowlink_ == unit.scc_ord_);
                u->state_ = Unit::k_analysed;
            } while (u != &unit);
        }
    }
}

Shared<Operation>
Recursive_Scope::make_function_setter(size_t nunits, Unit** units)
{
    Shared<const Phrase> syntax = nunits==1 ? units[0]->def_->syntax_ : syntax_;
    Shared<Module::Dictionary> nonlocal_dictionary = make<Module::Dictionary>();
    std::vector<Shared<Operation>> nonlocal_exprs;
    slot_t slot = 0;

    std::vector<Shared<Function_Definition>> funs;
    funs.reserve(nunits);
    for (size_t u = 0; u < nunits; ++u) {
        if (auto f = cast<Function_Definition>(units[u]->def_)) {
            funs.push_back(f);
            (*nonlocal_dictionary)[f->name_->symbol_] = slot++;
            nonlocal_exprs.push_back(
                make<Constant>(Shared<const Phrase>{f->lambda_phrase_}, Value{f->lambda_}));
        } else
            throw Exception(At_Phrase(*units[u]->def_->syntax_, *this),
                "recursive data definition");
    }

    for (size_t u = 0; u < nunits; ++u) {
        for (auto b : units[u]->nonlocals_) {
            if (nonlocal_dictionary->find(b.first) == nonlocal_dictionary->end()) {
                (*nonlocal_dictionary)[b.first] = slot++;
                nonlocal_exprs.push_back(b.second);
            }
        }
    }

    Shared<Enum_Module_Expr> nonlocals = make<Enum_Module_Expr>(
        syntax, nonlocal_dictionary, nonlocal_exprs);
    Shared<Function_Setter> setter =
        Function_Setter::make(nunits, syntax, executable_.module_slot_, nonlocals);
    for (size_t i = 0; i < nunits; ++i)
        setter->at(i) = {funs[i]->slot_, funs[i]->lambda_};
    return setter;
}

// How do I report illegal recursion? Eg,
// f->data->f
//   f()=x;
//   x=f();
// Report "illegal recursive reference" for either the x or f reference.
// Specifically, it's a recursive reference in a data definition that's bad.
// So "illegal recursive reference" for the f reference.

Shared<Meaning>
Recursive_Scope::single_lookup(const Identifier& id)
{
    auto b = dictionary_.find(id.symbol_);
    if (b != dictionary_.end()) {
        analyse_unit(units_[b->second.unit_index_], &id);
        if (target_is_module_) {
            return make<Module_Data_Ref>(
                share(id), executable_.module_slot_, b->second.slot_index_);
        } else {
            return make<Local_Data_Ref>(share(id), b->second.slot_index_,
                b->second.variable_);
        }
    }
    return nullptr;
}
Shared<Locative>
Recursive_Scope::single_lvar_lookup(const Identifier& id)
{
    auto b = dictionary_.find(id.symbol_);
    if (b != dictionary_.end()) {
        if (target_is_module_) {
            throw Exception(At_Phrase(id, *this),
                stringify(id.symbol_,": not assignable"));
        } else {
            b->second.variable_->is_mutable_ = true;
            return make<Local_Locative>(share(id), b->second.slot_index_);
        }
    }
    return nullptr;
}
Shared<Meaning>
Recursive_Scope::Function_Environ::single_lookup(const Identifier& id)
{
    Shared<Meaning> m = scope_.lookup(id);
    if (isa<Constant>(m))
        return m;
    if (auto expr = cast<Operation>(m)) {
        unit_.nonlocals_[id.symbol_] = expr;
        return make<Symbolic_Ref>(share(id));
    }
    return m;
}
void
Recursive_Scope::add_action(Shared<const Phrase> phrase)
{
    action_phrases_.push_back(phrase);
}
unsigned
Recursive_Scope::add_unit(Shared<Unitary_Definition> def)
{
    units_.emplace_back(def);
    return units_.size() - 1;
}

Shared<Module_Expr>
analyse_module(Definition& def, Environ& env)
{
    Recursive_Scope scope(env, true, def.syntax_);
    scope.analyse(def);
    return make<Scoped_Module_Expr>(def.syntax_,
        std::move(scope.executable_));
}

Function_Setter_Base::Element::Element(slot_t s, Shared<Lambda> l)
:
    slot_(s), lambda_(l)
{}

Function_Setter_Base::Element::Element() noexcept {}

} // namespace curv
