/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/erased_types.cpp
 * - HIR Expansion - Replace `impl Trait` with the real type
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"

namespace {

    void expand_erased_type(const Span& sp, const StaticTraitResolve& m_resolve, HIR::TypeRef& ty)
    {
        const auto& e = ty.data().as_ErasedType();

        HIR::TypeRef    new_ty;
        TU_MATCH_HDRA( (e.m_inner), { )
        TU_ARMA(Fcn, ee) {
            MonomorphState    monomorph_cb;
            auto val = m_resolve.get_value(sp, ee.m_origin, monomorph_cb);
            const auto& fcn = *val.as_Function();
            const auto& erased_types = fcn.m_code.m_erased_types;

            ASSERT_BUG(sp, ee.m_index < erased_types.size(), "Erased type index out of range for " << ee.m_origin << " - " << ee.m_index << " >= " << erased_types.size());
            const auto& tpl = erased_types[ee.m_index];

            new_ty = monomorph_cb.monomorph_type(sp, tpl);
            m_resolve.expand_associated_types(sp, new_ty);
            }
        TU_ARMA(Alias, ee) {
            if( ee->type == HIR::TypeRef() ) {
                ERROR(Span(), E0000, "Erased type alias " << ee->path << " never set?");
            }
            new_ty = ee->type.clone();
            }
        TU_ARMA(Known, ee) {
            new_ty = ee.clone();
            }
        }
        DEBUG("> " << ty << " => " << new_ty);
        ty = mv$(new_ty);
    }

    class ExprVisitor_Extract:
        public ::HIR::ExprVisitorDef
    {
        const StaticTraitResolve& m_resolve;

    public:
        ExprVisitor_Extract(const StaticTraitResolve& resolve):
            m_resolve(resolve)
        {
        }

        void visit_root(::HIR::ExprPtr& root)
        {
            root->visit(*this);
            visit_type(root->m_res_type);
            for(auto& ty : root.m_bindings)
                visit_type(ty);
            for(auto& ty : root.m_erased_types)
                visit_type(ty);
        }

        void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) override {
            assert(node_ptr);
            node_ptr->visit(*this);
            visit_type(node_ptr->m_res_type);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            static Span sp;

            if( ty.data().is_ErasedType() )
            {
                TRACE_FUNCTION_FR(ty, ty);

                expand_erased_type(sp, m_resolve, ty);

                // Recurse (TODO: Cleanly prevent infinite recursion - TRACE_FUNCTION does crude prevention)
                visit_type(ty);
            }
            else
            {
                ::HIR::ExprVisitorDef::visit_type(ty);
            }
        }
    };

    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
        const ::HIR::ItemPath* m_fcn_path = nullptr;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}

        void visit_expr(::HIR::ExprPtr& exp) override
        {
            if( exp )
            {
                ExprVisitor_Extract    ev(m_resolve);
                ev.visit_root( exp );
            }
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& fcn) override
        {
            m_fcn_path = &p;
            ::HIR::Visitor::visit_function(p, fcn);
            m_fcn_path = nullptr;
        }
    };
    class OuterVisitor_Fixup:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor_Fixup(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}

        void visit_type(::HIR::TypeRef& ty) override
        {
            static const Span   sp;
            if( ty.data().is_ErasedType() )
            {
                TRACE_FUNCTION_FR(ty, ty);

                expand_erased_type(sp, m_resolve, ty);

                // Recurse (TODO: Cleanly prevent infinite recursion - TRACE_FUNCTION does crude prevention)
                visit_type(ty);
            }
            else
            {
                ::HIR::Visitor::visit_type(ty);
            }
        }
    };
}

void HIR_Expand_ErasedType(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );

    OuterVisitor_Fixup  ov_fix(crate);
    ov_fix.visit_crate(crate);
}

