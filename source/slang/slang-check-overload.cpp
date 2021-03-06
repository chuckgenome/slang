// slang-check-overload.cpp
#include "slang-check-impl.h"

#include "slang-lookup.h"

// This file implements semantic checking logic related
// to resolving overloading call operations, by checking
// the applicability and relative priority of various candidates.

namespace Slang
{
    SemanticsVisitor::ParamCounts SemanticsVisitor::CountParameters(FilteredMemberRefList<ParamDecl> params)
    {
        ParamCounts counts = { 0, 0 };
        for (auto param : params)
        {
            counts.allowed++;

            // No initializer means no default value
            //
            // TODO(tfoley): The logic here is currently broken in two ways:
            //
            // 1. We are assuming that once one parameter has a default, then all do.
            //    This can/should be validated earlier, so that we can assume it here.
            //
            // 2. We are not handling the possibility of multiple declarations for
            //    a single function, where we'd need to merge default parameters across
            //    all the declarations.
            if (!param.getDecl()->initExpr)
            {
                counts.required++;
            }
        }
        return counts;
    }

    SemanticsVisitor::ParamCounts SemanticsVisitor::CountParameters(DeclRef<GenericDecl> genericRef)
    {
        ParamCounts counts = { 0, 0 };
        for (auto m : genericRef.getDecl()->Members)
        {
            if (auto typeParam = as<GenericTypeParamDecl>(m))
            {
                counts.allowed++;
                if (!typeParam->initType.Ptr())
                {
                    counts.required++;
                }
            }
            else if (auto valParam = as<GenericValueParamDecl>(m))
            {
                counts.allowed++;
                if (!valParam->initExpr)
                {
                    counts.required++;
                }
            }
        }
        return counts;
    }

    bool SemanticsVisitor::TryCheckOverloadCandidateArity(
        OverloadResolveContext&		context,
        OverloadCandidate const&	candidate)
    {
        UInt argCount = context.getArgCount();
        ParamCounts paramCounts = { 0, 0 };
        switch (candidate.flavor)
        {
        case OverloadCandidate::Flavor::Func:
            paramCounts = CountParameters(GetParameters(candidate.item.declRef.as<CallableDecl>()));
            break;

        case OverloadCandidate::Flavor::Generic:
            paramCounts = CountParameters(candidate.item.declRef.as<GenericDecl>());
            break;

        default:
            SLANG_UNEXPECTED("unknown flavor of overload candidate");
            break;
        }

        if (argCount >= paramCounts.required && argCount <= paramCounts.allowed)
            return true;

        // Emit an error message if we are checking this call for real
        if (context.mode != OverloadResolveContext::Mode::JustTrying)
        {
            if (argCount < paramCounts.required)
            {
                getSink()->diagnose(context.loc, Diagnostics::notEnoughArguments, argCount, paramCounts.required);
            }
            else
            {
                SLANG_ASSERT(argCount > paramCounts.allowed);
                getSink()->diagnose(context.loc, Diagnostics::tooManyArguments, argCount, paramCounts.allowed);
            }
        }

        return false;
    }

    bool SemanticsVisitor::TryCheckOverloadCandidateFixity(
        OverloadResolveContext&		context,
        OverloadCandidate const&	candidate)
    {
        auto expr = context.originalExpr;

        auto decl = candidate.item.declRef.decl;

        if(auto prefixExpr = as<PrefixExpr>(expr))
        {
            if(decl->HasModifier<PrefixModifier>())
                return true;

            if (context.mode != OverloadResolveContext::Mode::JustTrying)
            {
                getSink()->diagnose(context.loc, Diagnostics::expectedPrefixOperator);
                getSink()->diagnose(decl, Diagnostics::seeDefinitionOf, decl->getName());
            }

            return false;
        }
        else if(auto postfixExpr = as<PostfixExpr>(expr))
        {
            if(decl->HasModifier<PostfixModifier>())
                return true;

            if (context.mode != OverloadResolveContext::Mode::JustTrying)
            {
                getSink()->diagnose(context.loc, Diagnostics::expectedPostfixOperator);
                getSink()->diagnose(decl, Diagnostics::seeDefinitionOf, decl->getName());
            }

            return false;
        }
        else
        {
            return true;
        }

        return false;
    }

    bool SemanticsVisitor::TryCheckGenericOverloadCandidateTypes(
        OverloadResolveContext&	context,
        OverloadCandidate&		candidate)
    {
        auto genericDeclRef = candidate.item.declRef.as<GenericDecl>();

        // We will go ahead and hang onto the arguments that we've
        // already checked, since downstream validation might need
        // them.
        auto genSubst = new GenericSubstitution();
        candidate.subst = genSubst;
        auto& checkedArgs = genSubst->args;

        Index aa = 0;
        for (auto memberRef : getMembers(genericDeclRef))
        {
            if (auto typeParamRef = memberRef.as<GenericTypeParamDecl>())
            {
                if (aa >= context.argCount)
                {
                    return false;
                }
                auto arg = context.getArg(aa++);

                TypeExp typeExp;
                if (context.mode == OverloadResolveContext::Mode::JustTrying)
                {
                    typeExp = tryCoerceToProperType(TypeExp(arg));
                    if(!typeExp.type)
                    {
                        return false;
                    }
                }
                else
                {
                    typeExp = CoerceToProperType(TypeExp(arg));
                }
                checkedArgs.add(typeExp.type);
            }
            else if (auto valParamRef = memberRef.as<GenericValueParamDecl>())
            {
                auto arg = context.getArg(aa++);

                if (context.mode == OverloadResolveContext::Mode::JustTrying)
                {
                    ConversionCost cost = kConversionCost_None;
                    if (!canCoerce(GetType(valParamRef), arg->type, &cost))
                    {
                        return false;
                    }
                    candidate.conversionCostSum += cost;
                }

                arg = coerce(GetType(valParamRef), arg);
                auto val = ExtractGenericArgInteger(arg, context.mode == OverloadResolveContext::Mode::JustTrying ? nullptr : getSink());
                checkedArgs.add(val);
            }
            else
            {
                continue;
            }
        }

        // Okay, we've made it!
        return true;
    }

    bool SemanticsVisitor::TryCheckOverloadCandidateTypes(
        OverloadResolveContext&	context,
        OverloadCandidate&		candidate)
    {
        Index argCount = context.getArgCount();

        List<DeclRef<ParamDecl>> params;
        switch (candidate.flavor)
        {
        case OverloadCandidate::Flavor::Func:
            params = GetParameters(candidate.item.declRef.as<CallableDecl>()).ToArray();
            break;

        case OverloadCandidate::Flavor::Generic:
            return TryCheckGenericOverloadCandidateTypes(context, candidate);

        default:
            SLANG_UNEXPECTED("unknown flavor of overload candidate");
            break;
        }

        // Note(tfoley): We might have fewer arguments than parameters in the
        // case where one or more parameters had defaults.
        SLANG_RELEASE_ASSERT(argCount <= params.getCount());

        for (Index ii = 0; ii < argCount; ++ii)
        {
            auto& arg = context.getArg(ii);
            auto argType = context.getArgType(ii);
            auto param = params[ii];

            if (context.mode == OverloadResolveContext::Mode::JustTrying)
            {
                ConversionCost cost = kConversionCost_None;
                if( context.disallowNestedConversions )
                {
                    // We need an exact match in this case.
                    if(!GetType(param)->Equals(argType))
                        return false;
                }
                else if (!canCoerce(GetType(param), argType, &cost))
                {
                    return false;
                }
                candidate.conversionCostSum += cost;
            }
            else
            {
                arg = coerce(GetType(param), arg);
            }
        }
        return true;
    }

    bool SemanticsVisitor::TryCheckOverloadCandidateDirections(
        OverloadResolveContext&		/*context*/,
        OverloadCandidate const&	/*candidate*/)
    {
        // TODO(tfoley): check `in` and `out` markers, as needed.
        return true;
    }

    bool SemanticsVisitor::TryCheckOverloadCandidateConstraints(
        OverloadResolveContext&		context,
        OverloadCandidate const&	candidate)
    {
        // We only need this step for generics, so always succeed on
        // everything else.
        if(candidate.flavor != OverloadCandidate::Flavor::Generic)
            return true;

        auto genericDeclRef = candidate.item.declRef.as<GenericDecl>();
        SLANG_ASSERT(genericDeclRef); // otherwise we wouldn't be a generic candidate...

        // We should have the existing arguments to the generic
        // handy, so that we can construct a substitution list.

        auto subst = candidate.subst.as<GenericSubstitution>();
        SLANG_ASSERT(subst);

        subst->genericDecl = genericDeclRef.getDecl();
        subst->outer = genericDeclRef.substitutions.substitutions;

        for( auto constraintDecl : genericDeclRef.getDecl()->getMembersOfType<GenericTypeConstraintDecl>() )
        {
            auto subset = genericDeclRef.substitutions;
            subset.substitutions = subst;
            DeclRef<GenericTypeConstraintDecl> constraintDeclRef(
                constraintDecl, subset);

            auto sub = GetSub(constraintDeclRef);
            auto sup = GetSup(constraintDeclRef);

            auto subTypeWitness = tryGetSubtypeWitness(sub, sup);
            if(subTypeWitness)
            {
                subst->args.add(subTypeWitness);
            }
            else
            {
                if(context.mode != OverloadResolveContext::Mode::JustTrying)
                {
                    getSink()->diagnose(context.loc, Diagnostics::typeArgumentDoesNotConformToInterface, sub, sup);
                }
                return false;
            }
        }

        // Done checking all the constraints, hooray.
        return true;
    }

    void SemanticsVisitor::TryCheckOverloadCandidate(
        OverloadResolveContext&		context,
        OverloadCandidate&			candidate)
    {
        if (!TryCheckOverloadCandidateArity(context, candidate))
            return;

        candidate.status = OverloadCandidate::Status::ArityChecked;
        if (!TryCheckOverloadCandidateFixity(context, candidate))
            return;

        candidate.status = OverloadCandidate::Status::FixityChecked;
        if (!TryCheckOverloadCandidateTypes(context, candidate))
            return;

        candidate.status = OverloadCandidate::Status::TypeChecked;
        if (!TryCheckOverloadCandidateDirections(context, candidate))
            return;

        candidate.status = OverloadCandidate::Status::DirectionChecked;
        if (!TryCheckOverloadCandidateConstraints(context, candidate))
            return;

        candidate.status = OverloadCandidate::Status::Applicable;
    }

    RefPtr<Expr> SemanticsVisitor::createGenericDeclRef(
        RefPtr<Expr>            baseExpr,
        RefPtr<Expr>            originalExpr,
        RefPtr<GenericSubstitution>   subst)
    {
        auto baseDeclRefExpr = as<DeclRefExpr>(baseExpr);
        if (!baseDeclRefExpr)
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), baseExpr, "expected a reference to a generic declaration");
            return CreateErrorExpr(originalExpr);
        }
        auto baseGenericRef = baseDeclRefExpr->declRef.as<GenericDecl>();
        if (!baseGenericRef)
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), baseExpr, "expected a reference to a generic declaration");
            return CreateErrorExpr(originalExpr);
        }

        subst->genericDecl = baseGenericRef.getDecl();
        subst->outer = baseGenericRef.substitutions.substitutions;

        DeclRef<Decl> innerDeclRef(GetInner(baseGenericRef), subst);

        RefPtr<Expr> base;
        if (auto mbrExpr = as<MemberExpr>(baseExpr))
            base = mbrExpr->BaseExpression;

        return ConstructDeclRefExpr(
            innerDeclRef,
            base,
            originalExpr->loc);
    }

    RefPtr<Expr> SemanticsVisitor::CompleteOverloadCandidate(
        OverloadResolveContext&		context,
        OverloadCandidate&			candidate)
    {
        // special case for generic argument inference failure
        if (candidate.status == OverloadCandidate::Status::GenericArgumentInferenceFailed)
        {
            String callString = getCallSignatureString(context);
            getSink()->diagnose(
                context.loc,
                Diagnostics::genericArgumentInferenceFailed,
                callString);

            String declString = getDeclSignatureString(candidate.item);
            getSink()->diagnose(candidate.item.declRef, Diagnostics::genericSignatureTried, declString);
            goto error;
        }

        context.mode = OverloadResolveContext::Mode::ForReal;

        if (!TryCheckOverloadCandidateArity(context, candidate))
            goto error;

        if (!TryCheckOverloadCandidateFixity(context, candidate))
            goto error;

        if (!TryCheckOverloadCandidateTypes(context, candidate))
            goto error;

        if (!TryCheckOverloadCandidateDirections(context, candidate))
            goto error;

        if (!TryCheckOverloadCandidateConstraints(context, candidate))
            goto error;

        {
            auto baseExpr = ConstructLookupResultExpr(
                candidate.item, context.baseExpr, context.funcLoc);

            switch(candidate.flavor)
            {
            case OverloadCandidate::Flavor::Func:
                {
                    RefPtr<AppExprBase> callExpr = as<InvokeExpr>(context.originalExpr);
                    if(!callExpr)
                    {
                        callExpr = new InvokeExpr();
                        callExpr->loc = context.loc;

                        for(Index aa = 0; aa < context.argCount; ++aa)
                            callExpr->Arguments.add(context.getArg(aa));
                    }


                    callExpr->FunctionExpr = baseExpr;
                    callExpr->type = QualType(candidate.resultType);

                    // A call may yield an l-value, and we should take a look at the candidate to be sure
                    if(auto subscriptDeclRef = candidate.item.declRef.as<SubscriptDecl>())
                    {
                        const auto& decl = subscriptDeclRef.getDecl();
                        if (decl->getMembersOfType<SetterDecl>().isNonEmpty() || decl->getMembersOfType<RefAccessorDecl>().isNonEmpty())
                        {
                            callExpr->type.IsLeftValue = true;
                        }
                    }

                    // TODO: there may be other cases that confer l-value-ness

                    return callExpr;
                }

                break;

            case OverloadCandidate::Flavor::Generic:
                return createGenericDeclRef(
                    baseExpr,
                    context.originalExpr,
                    candidate.subst.as<GenericSubstitution>());
                break;

            default:
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), context.loc, "unknown overload candidate flavor");
                break;
            }
        }


    error:

        if(context.originalExpr)
        {
            return CreateErrorExpr(context.originalExpr.Ptr());
        }
        else
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), context.loc, "no original expression for overload result");
            return nullptr;
        }
    }

        /// Does the given `declRef` represent an interface requirement?
    bool isInterfaceRequirement(DeclRef<Decl> const& declRef)
    {
        if(!declRef)
            return false;

        auto parent = declRef.GetParent();
        if(parent.as<GenericDecl>())
            parent = parent.GetParent();

        if(parent.as<InterfaceDecl>())
            return true;

        return false;
    }

    int SemanticsVisitor::CompareLookupResultItems(
        LookupResultItem const& left,
        LookupResultItem const& right)
    {
        // It is possible for lookup to return both an interface requirement
        // and the concrete function that satisfies that requirement.
        // We always want to favor a concrete method over an interface
        // requirement it might override.
        //
        // TODO: This should turn into a more detailed check such that
        // a candidate for declaration A is always better than a candidate
        // for declaration B if A is an override of B. We can't
        // easily make that check right now because we aren't tracking
        // this kind of "is an override of ..." information on declarations
        // directly (it is only visible through the requirement witness
        // information for inheritance declarations).
        //
        bool leftIsInterfaceRequirement = isInterfaceRequirement(left.declRef);
        bool rightIsInterfaceRequirement = isInterfaceRequirement(right.declRef);
        if(leftIsInterfaceRequirement != rightIsInterfaceRequirement)
            return int(leftIsInterfaceRequirement) - int(rightIsInterfaceRequirement);

        // TODO: We should always have rules such that in a tie a declaration
        // A::m is better than B::m when all other factors are equal and
        // A inherits from B.

        // TODO: There are other cases like this we need to add in terms
        // of ranking/prioritizing overloads, around things like
        // "transparent" members, or when lookup proceeds from an "inner"
        // to an "outer" scope. In many cases the right way to proceed
        // could involve attaching a distance/cost/rank to things directly
        // as part of lookup, and in other cases it might be best handled
        // as a semantic check based on the actual declarations found.

        return 0;
    }

    int SemanticsVisitor::CompareOverloadCandidates(
        OverloadCandidate*	left,
        OverloadCandidate*	right)
    {
        // If one candidate got further along in validation, pick it
        if (left->status != right->status)
            return int(right->status) - int(left->status);

        // If both candidates are applicable, then we need to compare
        // the costs of their type conversion sequences
        if(left->status == OverloadCandidate::Status::Applicable)
        {
            if (left->conversionCostSum != right->conversionCostSum)
                return left->conversionCostSum - right->conversionCostSum;

            // If all conversion costs match, then we should consider
            // whether one of the two items/declarations should be
            // preferred based on grounds that have nothing to do
            // with applicability or conversion costs.
            //
            auto itemDiff = CompareLookupResultItems(left->item, right->item);
            if(itemDiff)
                return itemDiff;
        }

        return 0;
    }

    void SemanticsVisitor::AddOverloadCandidateInner(
        OverloadResolveContext& context,
        OverloadCandidate&		candidate)
    {
        // Filter our existing candidates, to remove any that are worse than our new one

        bool keepThisCandidate = true; // should this candidate be kept?

        if (context.bestCandidates.getCount() != 0)
        {
            // We have multiple candidates right now, so filter them.
            bool anyFiltered = false;
            // Note that we are querying the list length on every iteration,
            // because we might remove things.
            for (Index cc = 0; cc < context.bestCandidates.getCount(); ++cc)
            {
                int cmp = CompareOverloadCandidates(&candidate, &context.bestCandidates[cc]);
                if (cmp < 0)
                {
                    // our new candidate is better!

                    // remove it from the list (by swapping in a later one)
                    context.bestCandidates.fastRemoveAt(cc);
                    // and then reduce our index so that we re-visit the same index
                    --cc;

                    anyFiltered = true;
                }
                else if(cmp > 0)
                {
                    // our candidate is worse!
                    keepThisCandidate = false;
                }
            }
            // It should not be possible that we removed some existing candidate *and*
            // chose not to keep this candidate (otherwise the better-ness relation
            // isn't transitive). Therefore we confirm that we either chose to keep
            // this candidate (in which case filtering is okay), or we didn't filter
            // anything.
            SLANG_ASSERT(keepThisCandidate || !anyFiltered);
        }
        else if(context.bestCandidate)
        {
            // There's only one candidate so far
            int cmp = CompareOverloadCandidates(&candidate, context.bestCandidate);
            if(cmp < 0)
            {
                // our new candidate is better!
                context.bestCandidate = nullptr;
            }
            else if (cmp > 0)
            {
                // our candidate is worse!
                keepThisCandidate = false;
            }
        }

        // If our candidate isn't good enough, then drop it
        if (!keepThisCandidate)
            return;

        // Otherwise we want to keep the candidate
        if (context.bestCandidates.getCount() > 0)
        {
            // There were already multiple candidates, and we are adding one more
            context.bestCandidates.add(candidate);
        }
        else if (context.bestCandidate)
        {
            // There was a unique best candidate, but now we are ambiguous
            context.bestCandidates.add(*context.bestCandidate);
            context.bestCandidates.add(candidate);
            context.bestCandidate = nullptr;
        }
        else
        {
            // This is the only candidate worth keeping track of right now
            context.bestCandidateStorage = candidate;
            context.bestCandidate = &context.bestCandidateStorage;
        }
    }

    void SemanticsVisitor::AddOverloadCandidate(
        OverloadResolveContext& context,
        OverloadCandidate&		candidate)
    {
        // Try the candidate out, to see if it is applicable at all.
        TryCheckOverloadCandidate(context, candidate);

        // Now (potentially) add it to the set of candidate overloads to consider.
        AddOverloadCandidateInner(context, candidate);
    }

    void SemanticsVisitor::AddFuncOverloadCandidate(
        LookupResultItem			item,
        DeclRef<CallableDecl>             funcDeclRef,
        OverloadResolveContext&		context)
    {
        auto funcDecl = funcDeclRef.getDecl();
        ensureDecl(funcDecl, DeclCheckState::CanUseFuncSignature);

        // If this function is a redeclaration,
        // then we don't want to include it multiple times,
        // and mistakenly think we have an ambiguous call.
        //
        // Instead, we will carefully consider only the
        // "primary" declaration of any callable.
        if (auto primaryDecl = funcDecl->primaryDecl)
        {
            if (funcDecl != primaryDecl)
            {
                // This is a redeclaration, so we don't
                // want to consider it. The primary
                // declaration should also get considered
                // for the call site and it will match
                // anything this declaration would have
                // matched.
                return;
            }
        }

        OverloadCandidate candidate;
        candidate.flavor = OverloadCandidate::Flavor::Func;
        candidate.item = item;
        candidate.resultType = GetResultType(funcDeclRef);

        AddOverloadCandidate(context, candidate);
    }

    void SemanticsVisitor::AddFuncOverloadCandidate(
        RefPtr<FuncType>		/*funcType*/,
        OverloadResolveContext&	/*context*/)
    {
#if 0
        if (funcType->decl)
        {
            AddFuncOverloadCandidate(funcType->decl, context);
        }
        else if (funcType->Func)
        {
            AddFuncOverloadCandidate(funcType->Func->SyntaxNode, context);
        }
        else if (funcType->Component)
        {
            AddComponentFuncOverloadCandidate(funcType->Component, context);
        }
#else
        throw "unimplemented";
#endif
    }

    void SemanticsVisitor::AddCtorOverloadCandidate(
        LookupResultItem            typeItem,
        RefPtr<Type>                type,
        DeclRef<ConstructorDecl>    ctorDeclRef,
        OverloadResolveContext&     context,
        RefPtr<Type>                resultType)
    {
        ensureDecl(ctorDeclRef, DeclCheckState::CanUseFuncSignature);

        // `typeItem` refers to the type being constructed (the thing
        // that was applied as a function) so we need to construct
        // a `LookupResultItem` that refers to the constructor instead

        LookupResultItem ctorItem;
        ctorItem.declRef = ctorDeclRef;
        ctorItem.breadcrumbs = new LookupResultItem::Breadcrumb(
            LookupResultItem::Breadcrumb::Kind::Member,
            typeItem.declRef,
            typeItem.breadcrumbs);

        OverloadCandidate candidate;
        candidate.flavor = OverloadCandidate::Flavor::Func;
        candidate.item = ctorItem;
        candidate.resultType = resultType;

        AddOverloadCandidate(context, candidate);
    }

    DeclRef<Decl> SemanticsVisitor::SpecializeGenericForOverload(
        DeclRef<GenericDecl>    genericDeclRef,
        OverloadResolveContext& context)
    {
        ensureDecl(genericDeclRef, DeclCheckState::CanSpecializeGeneric);

        ConstraintSystem constraints;
        constraints.loc = context.loc;
        constraints.genericDecl = genericDeclRef.getDecl();

        // Construct a reference to the inner declaration that has any generic
        // parameter substitutions in place already, but *not* any substutions
        // for the generic declaration we are currently trying to infer.
        auto innerDecl = GetInner(genericDeclRef);
        DeclRef<Decl> unspecializedInnerRef = DeclRef<Decl>(innerDecl, genericDeclRef.substitutions);

        // Check what type of declaration we are dealing with, and then try
        // to match it up with the arguments accordingly...
        if (auto funcDeclRef = unspecializedInnerRef.as<CallableDecl>())
        {
            auto params = GetParameters(funcDeclRef).ToArray();

            Index argCount = context.getArgCount();
            Index paramCount = params.getCount();

            // Bail out on mismatch.
            // TODO(tfoley): need more nuance here
            if (argCount != paramCount)
            {
                return DeclRef<Decl>(nullptr, nullptr);
            }

            for (Index aa = 0; aa < argCount; ++aa)
            {
#if 0
                if (!TryUnifyArgAndParamTypes(constraints, args[aa], params[aa]))
                    return DeclRef<Decl>(nullptr, nullptr);
#else
                // The question here is whether failure to "unify" an argument
                // and parameter should lead to immediate failure.
                //
                // The case that is interesting is if we want to unify, say:
                // `vector<float,N>` and `vector<int,3>`
                //
                // It is clear that we should solve with `N = 3`, and then
                // a later step may find that the resulting types aren't
                // actually a match.
                //
                // A more refined approach to "unification" could of course
                // see that `int` can convert to `float` and use that fact.
                // (and indeed we already use something like this to unify
                // `float` and `vector<T,3>`)
                //
                // So the question is then whether a mismatch during the
                // unification step should be taken as an immediate failure...

                TryUnifyTypes(constraints, context.getArgType(aa), GetType(params[aa]));
#endif
            }
        }
        else
        {
            // TODO(tfoley): any other cases needed here?
            return DeclRef<Decl>(nullptr, nullptr);
        }

        auto constraintSubst = TrySolveConstraintSystem(&constraints, genericDeclRef);
        if (!constraintSubst)
        {
            // constraint solving failed
            return DeclRef<Decl>(nullptr, nullptr);
        }

        // We can now construct a reference to the inner declaration using
        // the solution to our constraints.
        return DeclRef<Decl>(innerDecl, constraintSubst);
    }

    void SemanticsVisitor::AddTypeOverloadCandidates(
        RefPtr<Type>            type,
        OverloadResolveContext&	context)
    {
        // The code being checked is trying to apply `type` like a function.
        // Semantically, the operations `T(args...)` is equivalent to
        // `T.__init(args...)` if we had a surface syntax that supported
        // looking up `__init` declarations by that name.
        //
        // Internally, all `__init` declarations are stored with the name
        // `$init`, to avoid potential conflicts if a user decided to name
        // a field/method `__init`.
        //
        // We will look up all the initializers on `type` by looking up
        // its members named `$init`, and then proceed to perform overload
        // resolution with what we find.
        //
        // TODO: One wrinkle here is single-argument constructor syntax.
        // An operation like `(T) oneArg` or `T(oneArg)` is currently
        // treated as a call expression, but we might want such cases
        // to go through the type coercion logic first/instead, because
        // by doing so we could weed out cases where a type is "constructed"
        // from a value of the same type. There is no need in Slang for
        // "copy constructors" but the stdlib currently has to define
        // some just to make code that does, e.g., `float(1.0f)` work.

        LookupResult initializers = lookUpMember(
            getSession(),
            this,
            getName("$init"),
            type);

        AddOverloadCandidates(initializers, context);
    }

    void SemanticsVisitor::AddDeclRefOverloadCandidates(
        LookupResultItem		item,
        OverloadResolveContext&	context)
    {
        auto declRef = item.declRef;

        if (auto funcDeclRef = item.declRef.as<CallableDecl>())
        {
            AddFuncOverloadCandidate(item, funcDeclRef, context);
        }
        else if (auto aggTypeDeclRef = item.declRef.as<AggTypeDecl>())
        {
            auto type = DeclRefType::Create(
                getSession(),
                aggTypeDeclRef);
            AddTypeOverloadCandidates(type, context);
        }
        else if (auto genericDeclRef = item.declRef.as<GenericDecl>())
        {
            // Try to infer generic arguments, based on the context
            DeclRef<Decl> innerRef = SpecializeGenericForOverload(genericDeclRef, context);

            if (innerRef)
            {
                // If inference works, then we've now got a
                // specialized declaration reference we can apply.

                LookupResultItem innerItem;
                innerItem.breadcrumbs = item.breadcrumbs;
                innerItem.declRef = innerRef;

                AddDeclRefOverloadCandidates(innerItem, context);
            }
            else
            {
                // If inference failed, then we need to create
                // a candidate that can be used to reflect that fact
                // (so we can report a good error)
                OverloadCandidate candidate;
                candidate.item = item;
                candidate.flavor = OverloadCandidate::Flavor::UnspecializedGeneric;
                candidate.status = OverloadCandidate::Status::GenericArgumentInferenceFailed;

                AddOverloadCandidateInner(context, candidate);
            }
        }
        else if( auto typeDefDeclRef = item.declRef.as<TypeDefDecl>() )
        {
            auto type = getNamedType(getSession(), typeDefDeclRef);
            AddTypeOverloadCandidates(type, context);
        }
        else if( auto genericTypeParamDeclRef = item.declRef.as<GenericTypeParamDecl>() )
        {
            auto type = DeclRefType::Create(
                getSession(),
                genericTypeParamDeclRef);
            AddTypeOverloadCandidates(type, context);
        }
        else
        {
            // TODO(tfoley): any other cases needed here?
        }
    }

    void SemanticsVisitor::AddOverloadCandidates(
        LookupResult const&     result,
        OverloadResolveContext&	context)
    {
        if(result.isOverloaded())
        {
            for(auto item : result.items)
            {
                AddDeclRefOverloadCandidates(item, context);
            }
        }
        else
        {
            AddDeclRefOverloadCandidates(result.item, context);
        }
    }

    void SemanticsVisitor::AddOverloadCandidates(
        RefPtr<Expr>	funcExpr,
        OverloadResolveContext&			context)
    {
        auto funcExprType = funcExpr->type;

        if (auto declRefExpr = as<DeclRefExpr>(funcExpr))
        {
            // The expression directly referenced a declaration,
            // so we can use that declaration directly to look
            // for anything applicable.
            AddDeclRefOverloadCandidates(LookupResultItem(declRefExpr->declRef), context);
        }
        else if (auto funcType = as<FuncType>(funcExprType))
        {
            // TODO(tfoley): deprecate this path...
            AddFuncOverloadCandidate(funcType, context);
        }
        else if (auto overloadedExpr = as<OverloadedExpr>(funcExpr))
        {
            AddOverloadCandidates(overloadedExpr->lookupResult2, context);
        }
        else if (auto overloadedExpr2 = as<OverloadedExpr2>(funcExpr))
        {
            for (auto item : overloadedExpr2->candidiateExprs)
            {
                AddOverloadCandidates(item, context);
            }
        }
        else if (auto typeType = as<TypeType>(funcExprType))
        {
            // If none of the above cases matched, but we are
            // looking at a type, then I suppose we have
            // a constructor call on our hands.
            //
            // TODO(tfoley): are there any meaningful types left
            // that aren't declaration references?
            auto type = typeType->type;
            AddTypeOverloadCandidates(type, context);
            return;
        }
    }

    void SemanticsVisitor::formatType(StringBuilder& sb, RefPtr<Type> type)
    {
        sb << type->ToString();
    }

    void SemanticsVisitor::formatVal(StringBuilder& sb, RefPtr<Val> val)
    {
        sb << val->ToString();
    }

    void SemanticsVisitor::formatDeclPath(StringBuilder& sb, DeclRef<Decl> declRef)
    {
        // Find the parent declaration
        auto parentDeclRef = declRef.GetParent();

        // If the immediate parent is a generic, then we probably
        // want the declaration above that...
        auto parentGenericDeclRef = parentDeclRef.as<GenericDecl>();
        if(parentGenericDeclRef)
        {
            parentDeclRef = parentGenericDeclRef.GetParent();
        }

        // Depending on what the parent is, we may want to format things specially
        if(auto aggTypeDeclRef = parentDeclRef.as<AggTypeDecl>())
        {
            formatDeclPath(sb, aggTypeDeclRef);
            sb << ".";
        }

        sb << getText(declRef.GetName());

        // If the parent declaration is a generic, then we need to print out its
        // signature
        if( parentGenericDeclRef )
        {
            auto genSubst = declRef.substitutions.substitutions.as<GenericSubstitution>();
            SLANG_RELEASE_ASSERT(genSubst);
            SLANG_RELEASE_ASSERT(genSubst->genericDecl == parentGenericDeclRef.getDecl());

            sb << "<";
            bool first = true;
            for(auto arg : genSubst->args)
            {
                if(!first) sb << ", ";
                formatVal(sb, arg);
                first = false;
            }
            sb << ">";
        }
    }

    void SemanticsVisitor::formatDeclParams(StringBuilder& sb, DeclRef<Decl> declRef)
    {
        if (auto funcDeclRef = declRef.as<CallableDecl>())
        {

            // This is something callable, so we need to also print parameter types for overloading
            sb << "(";

            bool first = true;
            for (auto paramDeclRef : GetParameters(funcDeclRef))
            {
                if (!first) sb << ", ";

                formatType(sb, GetType(paramDeclRef));

                first = false;

            }

            sb << ")";
        }
        else if(auto genericDeclRef = declRef.as<GenericDecl>())
        {
            sb << "<";
            bool first = true;
            for (auto paramDeclRef : getMembers(genericDeclRef))
            {
                if(auto genericTypeParam = paramDeclRef.as<GenericTypeParamDecl>())
                {
                    if (!first) sb << ", ";
                    first = false;

                    sb << getText(genericTypeParam.GetName());
                }
                else if(auto genericValParam = paramDeclRef.as<GenericValueParamDecl>())
                {
                    if (!first) sb << ", ";
                    first = false;

                    formatType(sb, GetType(genericValParam));
                    sb << " ";
                    sb << getText(genericValParam.GetName());
                }
                else
                {}
            }
            sb << ">";

            formatDeclParams(sb, DeclRef<Decl>(GetInner(genericDeclRef), genericDeclRef.substitutions));
        }
        else
        {
        }
    }

    void SemanticsVisitor::formatDeclSignature(StringBuilder& sb, DeclRef<Decl> declRef)
    {
        formatDeclPath(sb, declRef);
        formatDeclParams(sb, declRef);
    }

    String SemanticsVisitor::getDeclSignatureString(DeclRef<Decl> declRef)
    {
        StringBuilder sb;
        formatDeclSignature(sb, declRef);
        return sb.ProduceString();
    }

    String SemanticsVisitor::getDeclSignatureString(LookupResultItem item)
    {
        return getDeclSignatureString(item.declRef);
    }

    String SemanticsVisitor::getCallSignatureString(
        OverloadResolveContext&     context)
    {
        StringBuilder argsListBuilder;
        argsListBuilder << "(";

        UInt argCount = context.getArgCount();
        for( UInt aa = 0; aa < argCount; ++aa )
        {
            if(aa != 0) argsListBuilder << ", ";
            argsListBuilder << context.getArgType(aa)->ToString();
        }
        argsListBuilder << ")";
        return argsListBuilder.ProduceString();
    }

    RefPtr<Expr> SemanticsVisitor::ResolveInvoke(InvokeExpr * expr)
    {
        OverloadResolveContext context;
        // check if this is a stdlib operator call, if so we want to use cached results
        // to speed up compilation
        bool shouldAddToCache = false;
        OperatorOverloadCacheKey key;
        TypeCheckingCache* typeCheckingCache = getSession()->getTypeCheckingCache();
        if (auto opExpr = as<OperatorExpr>(expr))
        {
            if (key.fromOperatorExpr(opExpr))
            {
                OverloadCandidate candidate;
                if (typeCheckingCache->resolvedOperatorOverloadCache.TryGetValue(key, candidate))
                {
                    context.bestCandidateStorage = candidate;
                    context.bestCandidate = &context.bestCandidateStorage;
                }
                else
                {
                    shouldAddToCache = true;
                }
            }
        }

        // Look at the base expression for the call, and figure out how to invoke it.
        auto funcExpr = expr->FunctionExpr;
        auto funcExprType = funcExpr->type;

        // If we are trying to apply an erroneous expression, then just bail out now.
        if(IsErrorExpr(funcExpr))
        {
            return CreateErrorExpr(expr);
        }
        // If any of the arguments is an error, then we should bail out, to avoid
        // cascading errors where we successfully pick an overload, but not the one
        // the user meant.
        for (auto arg : expr->Arguments)
        {
            if (IsErrorExpr(arg))
                return CreateErrorExpr(expr);
        }

        context.originalExpr = expr;
        context.funcLoc = funcExpr->loc;

        context.argCount = expr->Arguments.getCount();
        context.args = expr->Arguments.getBuffer();
        context.loc = expr->loc;

        if (auto funcMemberExpr = as<MemberExpr>(funcExpr))
        {
            context.baseExpr = funcMemberExpr->BaseExpression;
        }
        else if (auto funcOverloadExpr = as<OverloadedExpr>(funcExpr))
        {
            context.baseExpr = funcOverloadExpr->base;
        }
        else if (auto funcOverloadExpr2 = as<OverloadedExpr2>(funcExpr))
        {
            context.baseExpr = funcOverloadExpr2->base;
        }

        // TODO: We should have a special case here where an `InvokeExpr`
        // with a single argument where the base/func expression names
        // a type should always be treated as an explicit type coercion
        // (and hence bottleneck through `coerce()`) instead of just
        // as a constructor call.
        //
        // Such a special-case would help us handle cases of identity
        // casts (casting an expression to the type it already has),
        // without needing dummy initializer/constructor declarations.
        //
        // Handling that special casing here (rather than in, say,
        // `visitTypeCastExpr`) would allow us to continue to ensure
        // that `(T) expr` and `T(expr)` continue to be semantically
        // equivalent in (almost) all cases.

        if (!context.bestCandidate)
        {
            AddOverloadCandidates(funcExpr, context);
        }

        if (context.bestCandidates.getCount() > 0)
        {
            // Things were ambiguous.

            // It might be that things were only ambiguous because
            // one of the argument expressions had an error, and
            // so a bunch of candidates could match at that position.
            //
            // If any argument was an error, we skip out on printing
            // another message, to avoid cascading errors.
            for (auto arg : expr->Arguments)
            {
                if (IsErrorExpr(arg))
                {
                    return CreateErrorExpr(expr);
                }
            }

            Name* funcName = nullptr;
            if (auto baseVar = as<VarExpr>(funcExpr))
                funcName = baseVar->name;
            else if(auto baseMemberRef = as<MemberExpr>(funcExpr))
                funcName = baseMemberRef->name;

            String argsList = getCallSignatureString(context);

            if (context.bestCandidates[0].status != OverloadCandidate::Status::Applicable)
            {
                // There were multiple equally-good candidates, but none actually usable.
                // We will construct a diagnostic message to help out.

                if (funcName)
                {
                    getSink()->diagnose(expr, Diagnostics::noApplicableOverloadForNameWithArgs, funcName, argsList);
                }
                else
                {
                    getSink()->diagnose(expr, Diagnostics::noApplicableWithArgs, argsList);
                }
            }
            else
            {
                // There were multiple applicable candidates, so we need to report them.

                if (funcName)
                {
                    getSink()->diagnose(expr, Diagnostics::ambiguousOverloadForNameWithArgs, funcName, argsList);
                }
                else
                {
                    getSink()->diagnose(expr, Diagnostics::ambiguousOverloadWithArgs, argsList);
                }
            }

            {
                Index candidateCount = context.bestCandidates.getCount();
                Index maxCandidatesToPrint = 10; // don't show too many candidates at once...
                Index candidateIndex = 0;
                for (auto candidate : context.bestCandidates)
                {
                    String declString = getDeclSignatureString(candidate.item);

//                        declString = declString + "[" + String(candidate.conversionCostSum) + "]";

#if 0
                    // Debugging: ensure that we don't consider multiple declarations of the same operation
                    if (auto decl = as<CallableDecl>(candidate.item.declRef.decl))
                    {
                        char buffer[1024];
                        sprintf_s(buffer, sizeof(buffer), "[this:%p, primary:%p, next:%p]",
                            decl,
                            decl->primaryDecl,
                            decl->nextDecl);
                        declString.append(buffer);
                    }
#endif

                    getSink()->diagnose(candidate.item.declRef, Diagnostics::overloadCandidate, declString);

                    candidateIndex++;
                    if (candidateIndex == maxCandidatesToPrint)
                        break;
                }
                if (candidateIndex != candidateCount)
                {
                    getSink()->diagnose(expr, Diagnostics::moreOverloadCandidates, candidateCount - candidateIndex);
                }
            }

            return CreateErrorExpr(expr);
        }
        else if (context.bestCandidate)
        {
            // There was one best candidate, even if it might not have been
            // applicable in the end.
            // We will report errors for this one candidate, then, to give
            // the user the most help we can.
            if (shouldAddToCache)
                typeCheckingCache->resolvedOperatorOverloadCache[key] = *context.bestCandidate;
            return CompleteOverloadCandidate(context, *context.bestCandidate);
        }
        else
        {
            // Nothing at all was found that we could even consider invoking
            getSink()->diagnose(expr->FunctionExpr, Diagnostics::expectedFunction, funcExprType);
            expr->type = QualType(getSession()->getErrorType());
            return expr;
        }
    }

    void SemanticsVisitor::AddGenericOverloadCandidate(
        LookupResultItem		baseItem,
        OverloadResolveContext&	context)
    {
        if (auto genericDeclRef = baseItem.declRef.as<GenericDecl>())
        {
            ensureDecl(genericDeclRef, DeclCheckState::CanSpecializeGeneric);

            OverloadCandidate candidate;
            candidate.flavor = OverloadCandidate::Flavor::Generic;
            candidate.item = baseItem;
            candidate.resultType = nullptr;

            AddOverloadCandidate(context, candidate);
        }
    }

    void SemanticsVisitor::AddGenericOverloadCandidates(
        RefPtr<Expr>	baseExpr,
        OverloadResolveContext&			context)
    {
        if(auto baseDeclRefExpr = as<DeclRefExpr>(baseExpr))
        {
            auto declRef = baseDeclRefExpr->declRef;
            AddGenericOverloadCandidate(LookupResultItem(declRef), context);
        }
        else if (auto overloadedExpr = as<OverloadedExpr>(baseExpr))
        {
            // We are referring to a bunch of declarations, each of which might be generic
            LookupResult result;
            for (auto item : overloadedExpr->lookupResult2.items)
            {
                AddGenericOverloadCandidate(item, context);
            }
        }
        else
        {
            // any other cases?
        }
    }

    RefPtr<Expr> SemanticsExprVisitor::visitGenericAppExpr(GenericAppExpr* genericAppExpr)
    {
        // Start by checking the base expression and arguments.
        auto& baseExpr = genericAppExpr->FunctionExpr;
        baseExpr = CheckTerm(baseExpr);
        auto& args = genericAppExpr->Arguments;
        for (auto& arg : args)
        {
            arg = CheckTerm(arg);
        }

        return checkGenericAppWithCheckedArgs(genericAppExpr);
    }

        /// Check a generic application where the operands have already been checked.
    RefPtr<Expr> SemanticsVisitor::checkGenericAppWithCheckedArgs(GenericAppExpr* genericAppExpr)
    {
        // We are applying a generic to arguments, but there might be multiple generic
        // declarations with the same name, so this becomes a specialized case of
        // overload resolution.

        auto& baseExpr = genericAppExpr->FunctionExpr;
        auto& args = genericAppExpr->Arguments;

        // If there was an error in the base expression,  or in any of
        // the arguments, then just bail.
        if (IsErrorExpr(baseExpr))
        {
            return CreateErrorExpr(genericAppExpr);
        }
        for (auto argExpr : args)
        {
            if (IsErrorExpr(argExpr))
            {
                return CreateErrorExpr(genericAppExpr);
            }
        }

        // Otherwise, let's start looking at how to find an overload...

        OverloadResolveContext context;
        context.originalExpr = genericAppExpr;
        context.funcLoc = baseExpr->loc;
        context.argCount = args.getCount();
        context.args = args.getBuffer();
        context.loc = genericAppExpr->loc;

        context.baseExpr = GetBaseExpr(baseExpr);

        AddGenericOverloadCandidates(baseExpr, context);

        if (context.bestCandidates.getCount() > 0)
        {
            // Things were ambiguous.
            if (context.bestCandidates[0].status != OverloadCandidate::Status::Applicable)
            {
                // There were multiple equally-good candidates, but none actually usable.
                // We will construct a diagnostic message to help out.

                // TODO(tfoley): print a reasonable message here...

                getSink()->diagnose(genericAppExpr, Diagnostics::unimplemented, "no applicable generic");

                return CreateErrorExpr(genericAppExpr);
            }
            else
            {
                // There were multiple viable candidates, but that isn't an error: we just need
                // to complete all of them and create an overloaded expression as a result.

                auto overloadedExpr = new OverloadedExpr2();
                overloadedExpr->base = context.baseExpr;
                for (auto candidate : context.bestCandidates)
                {
                    auto candidateExpr = CompleteOverloadCandidate(context, candidate);
                    overloadedExpr->candidiateExprs.add(candidateExpr);
                }
                return overloadedExpr;
            }
        }
        else if (context.bestCandidate)
        {
            // There was one best candidate, even if it might not have been
            // applicable in the end.
            // We will report errors for this one candidate, then, to give
            // the user the most help we can.
            return CompleteOverloadCandidate(context, *context.bestCandidate);
        }
        else
        {
            // Nothing at all was found that we could even consider invoking
            getSink()->diagnose(genericAppExpr, Diagnostics::unimplemented, "expected a generic");
            return CreateErrorExpr(genericAppExpr);
        }
    }

}
