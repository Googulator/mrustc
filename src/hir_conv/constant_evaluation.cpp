/*
 * Evaluate constants
 *
 * HACK - Should be replaced with a reentrant typeck/mir pass
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <algorithm>
#include <mir/mir.hpp>
#include <hir_typeck/common.hpp>    // Monomorph

namespace {
    typedef ::std::vector< ::std::pair< ::std::string, ::HIR::Static> > t_new_values;
    
    struct NewvalState {
        t_new_values&   newval_output;
        const ::HIR::ItemPath&  mod_path;
        ::std::string   name_prefix;
        
        NewvalState(t_new_values& newval_output, const ::HIR::ItemPath& mod_path, ::std::string prefix):
            newval_output(newval_output),
            mod_path(mod_path),
            name_prefix(prefix)
        {
        }
    };
    
    ::HIR::Literal evaluate_constant(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::HIR::ExprPtr& expr, ::std::vector< ::HIR::Literal> args={});
    
    ::HIR::Literal clone_literal(const ::HIR::Literal& v)
    {
        TU_MATCH(::HIR::Literal, (v), (e),
        (Invalid,
            return ::HIR::Literal();
            ),
        (List,
            ::std::vector< ::HIR::Literal>  vals;
            for(const auto& val : e) {
                vals.push_back( clone_literal(val) );
            }
            return ::HIR::Literal( mv$(vals) );
            ),
        (Variant,
            ::std::vector< ::HIR::Literal>  vals;
            for(const auto& val : e.vals) {
                vals.push_back( clone_literal(val) );
            }
            return ::HIR::Literal::make_Variant({ e.idx, mv$(vals) });
            ),
        (Integer,
            return ::HIR::Literal(e);
            ),
        (Float,
            return ::HIR::Literal(e);
            ),
        (BorrowOf,
            return ::HIR::Literal(e.clone());
            ),
        (String,
            return ::HIR::Literal(e);
            )
        )
        throw "";
    }
    
    TAGGED_UNION(EntPtr, NotFound,
        (NotFound, struct{}),
        (Function, const ::HIR::Function*),
        (Static, const ::HIR::Static*),
        (Constant, const ::HIR::Constant*),
        (Struct, const ::HIR::Struct*)
        );
    enum class EntNS {
        Type,
        Value
    };
    EntPtr get_ent_simplepath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, EntNS ns)
    {
        const ::HIR::Module* mod;
        if( path.m_crate_name != "" ) {
            ASSERT_BUG(sp, crate.m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
            mod = &crate.m_ext_crates.at(path.m_crate_name)->m_root_module;
        }
        else {
            mod = &crate.m_root_module;
        }
        
        for( unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
        {
            const auto& pc = path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find component " << i << " of " << path);
            }
            TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
            (
                BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
                ),
            (Module,
                mod = &e2;
                )
            )
        }
        
        switch( ns )
        {
        case EntNS::Value: {
            auto it = mod->m_value_items.find( path.m_components.back() );
            if( it == mod->m_value_items.end() ) {
                return EntPtr {};
            }
            
            TU_MATCH( ::HIR::ValueItem, (it->second->ent), (e),
            (Import,
                ),
            (StructConstant,
                ),
            (StructConstructor,
                ),
            (Function,
                return EntPtr { &e };
                ),
            (Constant,
                return EntPtr { &e };
                ),
            (Static,
                return EntPtr { &e };
                )
            )
            BUG(sp, "Path " << path << " pointed to a invalid item - " << it->second->ent.tag_str());
            } break;
        case EntNS::Type: {
            auto it = mod->m_mod_items.find( path.m_components.back() );
            if( it == mod->m_mod_items.end() ) {
                return EntPtr {};
            }
            
            TU_MATCH( ::HIR::TypeItem, (it->second->ent), (e),
            (Import,
                ),
            (Module,
                ),
            (Trait,
                ),
            (Struct,
                return &e;
                ),
            (Union,
                ),
            (Enum,
                ),
            (TypeAlias,
                )
            )
            BUG(sp, "Path " << path << " pointed to an invalid item - " << it->second->ent.tag_str());
            } break;
        }
        throw "";
    }
    EntPtr get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, EntNS ns)
    {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            return get_ent_simplepath(sp, crate, e.m_path, ns);
            ),
        (UfcsInherent,
            // Easy (ish)
            EntPtr rv;
            crate.find_type_impls(*e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                switch( ns )
                {
                case EntNS::Value:
                    {
                        auto fit = impl.m_methods.find(e.item);
                        if( fit != impl.m_methods.end() )
                        {
                            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                            rv = EntPtr { &fit->second.data };
                            return true;
                        }
                    }
                    {
                        auto it = impl.m_constants.find(e.item);
                        if( it != impl.m_constants.end() )
                        {
                            rv = EntPtr { &it->second.data };
                            return true;
                        }
                    }
                    break;
                case EntNS::Type:
                    break;
                }
                return false;
                });
            return rv;
            ),
        (UfcsKnown,
            EntPtr rv;
            crate.find_trait_impls(e.trait.m_path, *e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                // Hacky selection of impl.
                // - TODO: Specialisation
                // - TODO: Inference? (requires full typeck)
                switch( ns )
                {
                case EntNS::Value:
                    {
                        auto fit = impl.m_methods.find(e.item);
                        if( fit != impl.m_methods.end() )
                        {
                            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                            rv = EntPtr { &fit->second.data };
                            return true;
                        }
                    }
                    {
                        auto it = impl.m_constants.find(e.item);
                        if( it != impl.m_constants.end() )
                        {
                            rv = EntPtr { &it->second.data };
                            return true;
                        }
                    }
                    break;
                case EntNS::Type:
                    break;
                }
                return false;
                });
            return rv;
            ),
        (UfcsUnknown,
            // TODO: Are these valid at this point in compilation?
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
            )
        )
        throw "";
    }
    const ::HIR::Function& get_function(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path)
    {
        auto rv = get_ent_fullpath(sp, crate, path, EntNS::Value);
        TU_IFLET( EntPtr, rv, Function, e,
            return *e;
        )
        else {
            TODO(sp, "Could not find function for " << path << " - " << rv.tag_str());
        }
    }
    
    ::HIR::Literal evaluate_constant_hir(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::HIR::ExprNode& expr, ::std::vector< ::HIR::Literal> args)
    {
        struct Visitor:
            public ::HIR::ExprVisitor
        {
            const ::HIR::Crate& m_crate;
            NewvalState m_newval_state;
            
            ::std::vector< ::HIR::Literal>   m_values;
            
            ::HIR::TypeRef  m_rv_type;
            ::HIR::Literal  m_rv;
            
            Visitor(const ::HIR::Crate& crate, NewvalState newval_state):
                m_crate(crate),
                m_newval_state( mv$(newval_state) )
            {}
            
            void badnode(const ::HIR::ExprNode& node) const {
                ERROR(node.span(), E0000, "Node " << typeid(node).name() << " not allowed in constant expression");
            }
            
            void visit(::HIR::ExprNode_Block& node) override {
                TRACE_FUNCTION_F("_Block");
                
                for(const auto& e : node.m_nodes)
                {
                    e->visit(*this);
                }
            }
            void visit(::HIR::ExprNode_Return& node) override {
                TODO(node.span(), "ExprNode_Return");
            }
            void visit(::HIR::ExprNode_Let& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Loop& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_LoopControl& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Match& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_If& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_Assign& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_BinOp& node) override {
                TRACE_FUNCTION_F("_BinOp");
                node.m_left->visit(*this);
                auto left = mv$(m_rv);
                node.m_right->visit(*this);
                auto right = mv$(m_rv);
                
                if( left.tag() != right.tag() ) {
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Types mismatched - " << left.tag_str() << " != " << right.tag_str());
                }
                
                // Keep m_rv_type
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Comparisons");
                    break;
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:
                case ::HIR::ExprNode_BinOp::Op::BoolOr:
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Logicals");
                    break;

                case ::HIR::ExprNode_BinOp::Op::Add:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le + re); ),
                    (Float,     m_rv = ::HIR::Literal(le + re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Sub:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le - re); ),
                    (Float,     m_rv = ::HIR::Literal(le - re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Mul:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le * re); ),
                    (Float,     m_rv = ::HIR::Literal(le * re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Div:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le / re); ),
                    (Float,     m_rv = ::HIR::Literal(le / re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Mod:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le % re); ),
                    (Float,     ERROR(node.span(), E0000, "modulo operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::And:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le % re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise and operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Or:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le | re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise or operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Xor:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le ^ re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise xor operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shr:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le >> re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise shift right operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shl:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le << re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise shift left operator on float in constant"); )
                    )
                    break;
                }
            }
            void visit(::HIR::ExprNode_UniOp& node) override {
                TRACE_FUNCTION_FR("_UniOp", m_rv);
                node.m_value->visit(*this);
                auto val = mv$(m_rv);
                
                // Keep m_rv_type
                switch(node.m_op)
                {
                case ::HIR::ExprNode_UniOp::Op::Invert:
                    TU_MATCH_DEF(::HIR::Literal, (val), (e),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal::make_Integer(~e); ),
                    (Float,     ERROR(node.span(), E0000, "not operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_UniOp::Op::Negate:
                    TU_MATCH_DEF(::HIR::Literal, (val), (e),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(-e); ),
                    (Float,     m_rv = ::HIR::Literal(-e); )
                    )
                    break;
                }
            }
            void visit(::HIR::ExprNode_Borrow& node) override {
                node.m_value->visit(*this);
                auto val = mv$(m_rv);
                
                if( node.m_type != ::HIR::BorrowType::Shared ) {
                    ERROR(node.span(), E0000, "Only shared borrows are allowed in constants");
                }
                
                m_rv_type = ::HIR::TypeRef::new_borrow( node.m_type, mv$(m_rv_type) );
                // Create new static containing borrowed data
                auto name = FMT(m_newval_state.name_prefix << &node);
                m_newval_state.newval_output.push_back(::std::make_pair( name, ::HIR::Static {
                    false,
                    ::HIR::TypeRef(),
                    ::HIR::ExprNodeP(),
                    mv$(val)
                    } ));
                m_rv = ::HIR::Literal::make_BorrowOf( (m_newval_state.mod_path + name).get_simple_path() );
            }
            void visit(::HIR::ExprNode_Cast& node) override {
                TRACE_FUNCTION_F("_Cast");
                node.m_value->visit(*this);
                auto val = mv$(m_rv);
                //DEBUG("ExprNode_Cast - val = " << val << " as " << node.m_type);
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (node.m_res_type.m_data), (te),
                (
                    m_rv = mv$(val);
                    ),
                (Primitive,
                    switch(te)
                    {
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        TU_MATCH_DEF( ::HIR::Literal, (val), (ve),
                        ( BUG(node.span(), "Cast to float, bad literal " << val.tag_str()); ),
                        (Float,
                            m_rv = mv$(val);
                            ),
                        (Integer,
                            m_rv = ::HIR::Literal(static_cast<double>(ve));
                            )
                        )
                        break;
                    case ::HIR::CoreType::I8:   case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::I16:  case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::I32:  case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::I64:  case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::Isize:  case ::HIR::CoreType::Usize:
                        TU_MATCH_DEF( ::HIR::Literal, (val), (ve),
                        ( BUG(node.span(), "Cast to float, bad literal " << val.tag_str()); ),
                        (Integer,
                            m_rv = mv$(val);
                            ),
                        (Float,
                            m_rv = ::HIR::Literal(static_cast<uint64_t>(ve));
                            )
                        )
                        break;
                    default:
                        m_rv = mv$(val);
                        break;
                    }
                    )
                )
                m_rv_type = node.m_res_type.clone();
            }
            void visit(::HIR::ExprNode_Unsize& node) override {
                TRACE_FUNCTION_F("_Unsize");
                node.m_value->visit(*this);
                //auto val = mv$(m_rv);
                //DEBUG("ExprNode_Unsize - val = " << val << " as " << node.m_type);
                m_rv_type = node.m_res_type.clone();
            }
            void visit(::HIR::ExprNode_Index& node) override {
                // Index
                node.m_index->visit(*this);
                if( !m_rv.is_Integer() )
                    ERROR(node.span(), E0000, "Array index isn't an integer - got " << m_rv.tag_str());
                auto idx = m_rv.as_Integer();
                
                // Value
                node.m_value->visit(*this);
                if( !m_rv.is_List() )
                    ERROR(node.span(), E0000, "Indexed value isn't a list - got " << m_rv.tag_str());
                auto v = mv$( m_rv.as_List() );
                
                // -> Perform
                if( idx >= v.size() )
                    ERROR(node.span(), E0000, "Constant array index " << idx << " out of range " << v.size());
                m_rv = mv$(v[idx]);
                
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (m_rv_type.m_data), (e),
                (
                    ERROR(node.span(), E0000, "Indexing non-array - " << m_rv_type);
                    ),
                (Array,
                    auto tmp = mv$(e.inner);
                    m_rv_type = mv$(*tmp);
                    )
                )
            }
            void visit(::HIR::ExprNode_Deref& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Emplace& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_TupleVariant& node) override {
                
                ::std::vector< ::HIR::Literal>  vals;
                for(const auto& vn : node.m_args ) {
                    vn->visit(*this);
                    assert( !m_rv.is_Invalid() );
                    vals.push_back( mv$(m_rv) );
                }
                
                if( node.m_is_struct )
                {
                    const auto& ent = m_crate.get_typeitem_by_path(node.span(), node.m_path.m_path);
                    ASSERT_BUG(node.span(), ent.is_Struct(), "_TupleVariant with m_is_struct set pointing to " << ent.tag_str());
                    const auto& str = ent.as_Struct();
                    
                    m_rv = ::HIR::Literal::make_List(mv$(vals));
                    m_rv_type = ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding(&str) );
                }
                else
                {
                    const auto& varname = node.m_path.m_path.m_components.back();
                    auto tmp_path = node.m_path.m_path;
                    tmp_path.m_components.pop_back();
                    const auto& ent = m_crate.get_typeitem_by_path(node.span(), tmp_path);
                    ASSERT_BUG(node.span(), ent.is_Enum(), "_TupleVariant with m_is_struct clear pointing to " << ent.tag_str());
                    const auto& enm = ent.as_Enum();
                    
                    auto it = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&x){ return x.first == varname; } );
                    ASSERT_BUG(node.span(), it != enm.m_variants.end(), "_TupleVariant points to unknown variant - " << node.m_path);
                    unsigned int var_idx = it - enm.m_variants.begin();

                    m_rv = ::HIR::Literal::make_Variant({var_idx, mv$(vals)});
                    m_rv_type = ::HIR::TypeRef::new_path( mv$(tmp_path), ::HIR::TypeRef::TypePathBinding(&enm) );
                }
            }
            void visit(::HIR::ExprNode_CallPath& node) override {
                TRACE_FUNCTION_FR("_CallPath - " << node.m_path, m_rv);
                auto& fcn = get_function(node.span(), m_crate, node.m_path);
                // TODO: Set m_const during parse
                //if( ! fcn.m_const ) {
                //    ERROR(node.span(), E0000, "Calling non-const function in const context - " << node.m_path);
                //}
                if( fcn.m_args.size() != node.m_args.size() ) {
                    ERROR(node.span(), E0000, "Incorrect argument count for " << node.m_path << " - expected " << fcn.m_args.size() << ", got " << node.m_args.size());
                }
                ::std::vector< ::HIR::Literal>  args;
                args.reserve( fcn.m_args.size() );
                for(unsigned int i = 0; i < fcn.m_args.size(); i ++ )
                {
                    const auto& pattern = fcn.m_args[i].first;
                    node.m_args[i]->visit(*this);
                    args.push_back( mv$(m_rv) );
                    TU_IFLET(::HIR::Pattern::Data, pattern.m_data, Any, e,
                        // Good
                    )
                    else {
                        ERROR(node.span(), E0000, "Constant functions can't have destructuring pattern argments");
                    }
                }
                
                // Call by invoking evaluate_constant on the function
                {
                    TRACE_FUNCTION_F("Call const fn " << node.m_path << " args={ " << args << " }");
                    m_rv = evaluate_constant(node.span(), m_crate, m_newval_state,  fcn.m_code, mv$(args));
                }
            }
            void visit(::HIR::ExprNode_CallValue& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_CallMethod& node) override {
                // TODO: const methods
                badnode(node);
            }
            void visit(::HIR::ExprNode_Field& node) override {
                const auto& sp = node.span();
                TRACE_FUNCTION_FR("_Field", m_rv);
                
                node.m_value->visit(*this);
                auto val = mv$( m_rv );
                
                if( !val.is_List() )
                    ERROR(sp, E0000, "Field access on invalid literal type - " << val.tag_str());
                auto& vals = val.as_List();
                
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (m_rv_type.m_data), (e),
                (
                    ERROR(sp, E0000, "Field access on invalid type - " << m_rv_type);
                    ),
                (Path,
                    TU_MATCHA( (e.binding), (pbe),
                    (Unbound,
                        ERROR(sp, E0000, "Field access on invalid type - " << m_rv_type);
                        ),
                    (Opaque,
                        ERROR(sp, E0000, "Field access on invalid type - " << m_rv_type);
                        ),
                    (Struct,
                        auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, &e.path.m_data.as_Generic().m_params, nullptr);
                        const auto& str = *pbe;
                        unsigned int idx=0;
                        TU_MATCHA( (str.m_data), (se),
                        (Unit,
                            ERROR(sp, E0000, "Field access on invalid type - " << m_rv_type << " - Unit-like");
                            ),
                        (Tuple,
                            idx = ::std::atoi( node.m_field.c_str() );
                            ASSERT_BUG(sp, idx < se.size(), "Index out of range in tuple struct");
                            m_rv_type = monomorphise_type_with(sp, se[idx].ent, monomorph_cb);
                            ),
                        (Named,
                            idx = ::std::find_if(se.begin(), se.end(), [&](const auto&x){return x.first==node.m_field;}) - se.begin();
                            ASSERT_BUG(sp, idx < se.size(), "Field no found in struct");
                            m_rv_type = monomorphise_type_with(sp, se[idx].second.ent, monomorph_cb);
                            )
                        )
                        ASSERT_BUG(sp, idx < vals.size(), "Index out of range in literal");
                        m_rv = mv$( vals[idx] );
                        ),
                    (Enum,
                        TODO(sp, "Field access on enum variant - " << m_rv_type);
                        ),
                    (Union,
                        TODO(sp, "Field access on union - " << m_rv_type);
                        )
                    )
                    ),
                (Tuple,
                    unsigned int idx = ::std::atoi( node.m_field.c_str() );
                    ASSERT_BUG(sp, idx < e.size(), "Index out of range in tuple");
                    ASSERT_BUG(sp, idx < vals.size(), "Index out of range in literal");
                    
                    m_rv = mv$( vals[idx] );
                    m_rv_type = mv$( e[idx] );
                    )
                )
            }

            void visit(::HIR::ExprNode_Literal& node) override {
                TRACE_FUNCTION_FR("_Literal", m_rv);
                TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
                (Integer,
                    m_rv = ::HIR::Literal(e.m_value);
                    ),
                (Float,
                    m_rv = ::HIR::Literal(e.m_value);
                    ),
                (Boolean,
                    m_rv = ::HIR::Literal(static_cast<uint64_t>(e));
                    ),
                (String,
                    m_rv = ::HIR::Literal(e);
                    ),
                (ByteString,
                    m_rv = ::HIR::Literal::make_String({e.begin(), e.end()});
                    )
                )
                m_rv_type = node.m_res_type.clone();
            }
            void visit(::HIR::ExprNode_UnitVariant& node) override {
                if( node.m_is_struct )
                {
                    const auto& ent = m_crate.get_typeitem_by_path(node.span(), node.m_path.m_path);
                    ASSERT_BUG(node.span(), ent.is_Struct(), "_UnitVariant with m_is_struct set pointing to " << ent.tag_str());
                    const auto& str = ent.as_Struct();
                    
                    m_rv = ::HIR::Literal::make_List({});
                    m_rv_type = ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding(&str) );
                }
                else
                {
                    const auto& varname = node.m_path.m_path.m_components.back();
                    auto tmp_path = node.m_path.m_path;
                    tmp_path.m_components.pop_back();
                    const auto& ent = m_crate.get_typeitem_by_path(node.span(), tmp_path);
                    ASSERT_BUG(node.span(), ent.is_Enum(), "_UnitVariant with m_is_struct clear pointing to " << ent.tag_str());
                    const auto& enm = ent.as_Enum();
                    
                    auto it = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&x){ return x.first == varname; } );
                    ASSERT_BUG(node.span(), it != enm.m_variants.end(), "_UnitVariant points to unknown variant - " << node.m_path);
                    unsigned int var_idx = it - enm.m_variants.begin();

                    m_rv = ::HIR::Literal::make_Variant({var_idx, {}});
                    m_rv_type = ::HIR::TypeRef::new_path( mv$(tmp_path), ::HIR::TypeRef::TypePathBinding(&enm) );
                }
            }
            void visit(::HIR::ExprNode_PathValue& node) override {
                TRACE_FUNCTION_FR("_PathValue - " << node.m_path, m_rv);
                auto ep = get_ent_fullpath(node.span(), m_crate, node.m_path, EntNS::Value);
                TU_MATCH_DEF( EntPtr, (ep), (e),
                (
                    BUG(node.span(), "Path value with unsupported value type - " << ep.tag_str());
                    ),
                (Static,
                    // TODO: Should be a more complex path to support associated paths
                    ASSERT_BUG(node.span(), node.m_path.m_data.is_Generic(), "Static path not Path::Generic - " << node.m_path);
                    m_rv = ::HIR::Literal(node.m_path.m_data.as_Generic().m_path);
                    m_rv_type = e->m_type.clone();
                    ),
                (Function,
                    // TODO: Should be a more complex path to support associated paths
                    ASSERT_BUG(node.span(), node.m_path.m_data.is_Generic(), "Function path not Path::Generic - " << node.m_path);
                    m_rv = ::HIR::Literal(node.m_path.m_data.as_Generic().m_path);
                    m_rv_type = ::HIR::TypeRef();   // TODO: Better type
                    ),
                (Constant,
                    // TODO: Associated constants
                    const auto& c = *e;
                    if( c.m_value_res.is_Invalid() ) {
                        const_cast<HIR::ExprNode&>(*c.m_value).visit(*this);
                    }
                    else {
                        m_rv = clone_literal(c.m_value_res);
                    }
                    m_rv_type = e->m_type.clone();
                    )
                )
            }
            void visit(::HIR::ExprNode_Variable& node) override {
                TRACE_FUNCTION_FR("_Variable - " << node.m_name, m_rv);
                // TODO: use the binding?
                if( node.m_slot >= m_values.size() ) {
                    ERROR(node.span(), E0000, "Couldn't find variable #" << node.m_slot << " " << node.m_name);
                }
                auto& v = m_values.at( node.m_slot );
                TU_MATCH_DEF(::HIR::Literal, (v), (e),
                (
                    m_rv = mv$(v);
                    ),
                (Integer,
                    m_rv = ::HIR::Literal(e);
                    ),
                (Float,
                    m_rv = ::HIR::Literal(e);
                    )
                )
                m_rv_type = ::HIR::TypeRef();    // TODO:
            }
            
            void visit(::HIR::ExprNode_StructLiteral& node) override {
                TRACE_FUNCTION_FR("_StructLiteral - " << node.m_path, m_rv);
                
                if( node.m_is_struct )
                {
                    const auto& ent = m_crate.get_typeitem_by_path(node.span(), node.m_path.m_path);
                    ASSERT_BUG(node.span(), ent.is_Struct(), "_StructLiteral with m_is_struct set pointing to a " << ent.tag_str());
                    const auto& str = ent.as_Struct();
                    const auto& fields = str.m_data.as_Named();
                    
                    ::std::vector< ::HIR::Literal>  vals;
                    if( node.m_base_value ) {
                        node.m_base_value->visit(*this);
                        auto base_val = mv$(m_rv);
                        if( !base_val.is_List() || base_val.as_List().size() != fields.size() ) {
                            BUG(node.span(), "Struct literal base value had an incorrect field count");
                        }
                        vals = mv$(base_val.as_List());
                    }
                    else {
                        vals.resize( fields.size() );
                    }
                    for( const auto& val_set : node.m_values ) {
                        unsigned int idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& v) { return v.first == val_set.first; } ) - fields.begin();
                        if( idx == fields.size() ) {
                            ERROR(node.span(), E0000, "Field name " << val_set.first << " isn't a member of " << node.m_path);
                        }
                        val_set.second->visit(*this);
                        vals[idx] = mv$(m_rv);
                    }
                    for( unsigned int i = 0; i < vals.size(); i ++ ) {
                        const auto& val = vals[i];
                        if( val.is_Invalid() ) {
                            ERROR(node.span(), E0000, "Field " << fields[i].first << " wasn't set");
                        }
                    }

                    m_rv = ::HIR::Literal::make_List(mv$(vals));
                    m_rv_type = ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding(&str) );
                }
                else
                {
                    const auto& ent = m_crate.get_typeitem_by_path(node.span(), node.m_path.m_path);
                    ASSERT_BUG(node.span(), ent.is_Enum(), "_StructLiteral with m_is_struct clear pointing to a " << ent.tag_str());
                    
                    TODO(node.span(), "Handle Enum _UnitVariant - " << node.m_path);
                }
            }
            void visit(::HIR::ExprNode_UnionLiteral& node) override {
                TRACE_FUNCTION_FR("_UnionLiteral - " << node.m_path, m_rv);
                TODO(node.span(), "_UnionLiteral");
            }
            void visit(::HIR::ExprNode_Tuple& node) override {
                ::std::vector< ::HIR::Literal>  vals;
                ::std::vector< ::HIR::TypeRef>  tys;
                for(const auto& vn : node.m_vals ) {
                    vn->visit(*this);
                    assert( !m_rv.is_Invalid() );
                    vals.push_back( mv$(m_rv) );
                    tys.push_back( mv$(m_rv_type) );
                }
                m_rv = ::HIR::Literal::make_List(mv$(vals));
                m_rv_type = ::HIR::TypeRef( mv$(tys) );
            }
            void visit(::HIR::ExprNode_ArrayList& node) override {
                ::std::vector< ::HIR::Literal>  vals;
                for(const auto& vn : node.m_vals ) {
                    vn->visit(*this);
                    assert( !m_rv.is_Invalid() );
                    vals.push_back( mv$(m_rv) );
                }
                m_rv = ::HIR::Literal::make_List(mv$(vals));
                m_rv_type = ::HIR::TypeRef::new_array( mv$(m_rv_type), vals.size() );
            }
            void visit(::HIR::ExprNode_ArraySized& node) override {
                node.m_size->visit(*this);
                assert( m_rv.is_Integer() );
                unsigned int count = static_cast<unsigned int>(m_rv.as_Integer());
                
                ::std::vector< ::HIR::Literal>  vals;
                vals.reserve( count );
                if( count > 0 )
                {
                    node.m_val->visit(*this);
                    assert( !m_rv.is_Invalid() );
                    for(unsigned int i = 0; i < count-1; i ++)
                    {
                        vals.push_back( clone_literal(m_rv) );
                    }
                    vals.push_back( mv$(m_rv) );
                }
                m_rv = ::HIR::Literal::make_List(mv$(vals));
                m_rv_type = ::HIR::TypeRef::new_array( mv$(m_rv_type), count );
            }
            
            void visit(::HIR::ExprNode_Closure& node) override {
                badnode(node);
            }
        };
        
        Visitor v { crate, newval_state };
        for(auto& arg : args)
            v.m_values.push_back( mv$(arg) );
        const_cast<::HIR::ExprNode&>(expr).visit(v);
        
        if( v.m_rv.is_Invalid() ) {
            BUG(sp, "Expression did not yeild a literal");
        }
        
        return mv$(v.m_rv);
    }
    
    ::HIR::Literal evaluate_constant_mir(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::MIR::Function& fcn, ::std::vector< ::HIR::Literal> args)
    {
        ::HIR::Literal  retval;
        ::std::vector< ::HIR::Literal>  locals;
        ::std::vector< ::HIR::Literal>  temps;
        locals.resize( fcn.named_variables.size() );
        temps.resize( fcn.temporaries.size() );
        
        auto get_lval = [&](const ::MIR::LValue& lv) -> ::HIR::Literal& {
            TU_MATCHA( (lv), (e),
            (Variable,
                if( e >= locals.size() )
                    BUG(sp, "Local index out of range - " << e << " >= " << locals.size());
                return locals[e];
                ),
            (Temporary,
                if( e.idx >= temps.size() )
                    BUG(sp, "Temp index out of range - " << e.idx << " >= " << temps.size());
                return temps[e.idx];
                ),
            (Argument,
                return args[e.idx];
                ),
            (Static,
                TODO(sp, "LValue::Static");
                ),
            (Return,
                return retval;
                ),
            (Field,
                TODO(sp, "LValue::Field");
                ),
            (Deref,
                TODO(sp, "LValue::Deref");
                ),
            (Index,
                TODO(sp, "LValue::Index");
                ),
            (Downcast,
                TODO(sp, "LValue::Downcast");
                )
            )
            throw "";
            };
        auto read_lval = [&](const ::MIR::LValue& lv) -> ::HIR::Literal {
            auto& v = get_lval(lv);
            TU_MATCH_DEF(::HIR::Literal, (v), (e),
            (
                return mv$(v);
                ),
            (Invalid,
                BUG(sp, "Read of invalid lvalue - " << lv);
                ),
            (BorrowOf,
                return ::HIR::Literal(e.clone());
                ),
            (Integer,
                return ::HIR::Literal(e);
                ),
            (Float,
                return ::HIR::Literal(e);
                )
            )
            };
        
        unsigned int cur_block = 0;
        for(;;)
        {
            const auto& block = fcn.blocks[cur_block];
            for(const auto& stmt : block.statements)
            {
                if( ! stmt.is_Assign() ) {
                    BUG(sp, "Non-assign statement - drop " << stmt.as_Drop().slot);
                }
                
                ::HIR::Literal  val;
                const auto& sa = stmt.as_Assign();
                TU_MATCHA( (sa.src), (e),
                (Use,
                    val = read_lval(e);
                    ),
                (Constant,
                    TU_MATCH(::MIR::Constant, (e), (e2),
                    (Int,
                        val = ::HIR::Literal(static_cast<uint64_t>(e2));
                        ),
                    (Uint,
                        val = ::HIR::Literal(e2);
                        ),
                    (Float,
                        val = ::HIR::Literal(e2);
                        ),
                    (Bool,
                        val = ::HIR::Literal(static_cast<uint64_t>(e2));
                        ),
                    (Bytes,
                        val = ::HIR::Literal::make_String({e2.begin(), e2.end()});
                        ),
                    (StaticString,
                        val = ::HIR::Literal(e2);
                        ),
                    (Const,
                        auto ent = get_ent_fullpath(sp, crate, e2.p, EntNS::Value);
                        ASSERT_BUG(sp, ent.is_Constant(), "MIR Constant::Const("<<e2.p<<") didn't point to a Constant - " << ent.tag_str());
                        val = clone_literal( ent.as_Constant()->m_value_res );
                        ),
                    (ItemAddr,
                        val = ::HIR::Literal::make_BorrowOf( e2.clone() );
                        )
                    )
                    ),
                (SizedArray,
                    ::std::vector< ::HIR::Literal>  vals;
                    if( e.count > 0 )
                    {
                        vals.reserve( e.count );
                        val = read_lval(e.val);
                        for(unsigned int i = 1; i < e.count; i++)
                            vals.push_back( clone_literal(val) );
                        vals.push_back( mv$(val) );
                    }
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    ),
                (Borrow,
                    TODO(sp, "RValue::Borrow");
                    ),
                (Cast,
                    auto inval = read_lval(e.val);
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (e.type.m_data), (te),
                    (
                        // NOTE: Can be an unsizing!
                        TODO(sp, "RValue::Cast to " << e.type << ", val = " << inval);
                        ),
                    (Primitive,
                        uint64_t mask;
                        switch(te)
                        {
                        // Integers mask down
                        case ::HIR::CoreType::I8:
                        case ::HIR::CoreType::U8:
                            mask = 0xFF;
                            if(0)
                        case ::HIR::CoreType::I16:
                        case ::HIR::CoreType::U16:
                            mask = 0xFFFF;
                            if(0)
                        case ::HIR::CoreType::I32:
                        case ::HIR::CoreType::U32:
                            mask = 0xFFFFFFFF;
                            if(0)
                        case ::HIR::CoreType::I64:
                        case ::HIR::CoreType::U64:
                            mask = 0xFFFFFFFFFFFFFFFF;
                        
                            TU_IFLET( ::HIR::Literal, inval, Integer, i,
                                val = ::HIR::Literal(i & mask);
                            )
                            else TU_IFLET( ::HIR::Literal, inval, Float, i,
                                val = ::HIR::Literal( static_cast<uint64_t>(i) & mask);
                            )
                            else {
                                BUG(sp, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                            }
                            break;
                        default:
                            TODO(sp, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        ),
                    // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
                    (Pointer,
                        TU_IFLET( ::HIR::Literal, inval, Integer, i,
                            val = ::HIR::Literal(i);
                        )
                        else {
                            BUG(sp, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        )
                    )
                    ),
                (BinOp,
                    auto inval_l = read_lval(e.val_l);
                    auto inval_r = read_lval(e.val_r);
                    ASSERT_BUG(sp, inval_l.tag() == inval_r.tag(), "Mismatched literal types in binop - " << inval_l << " and " << inval_r);
                    TU_MATCH_DEF( ::HIR::Literal, (inval_l, inval_r), (l, r),
                    (
                        TODO(sp, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
                        ),
                    (Integer,
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    val = ::HIR::Literal( l + r );  break;
                        case ::MIR::eBinOp::SUB:    val = ::HIR::Literal( l - r );  break;
                        case ::MIR::eBinOp::MUL:    val = ::HIR::Literal( l * r );  break;
                        case ::MIR::eBinOp::DIV:    val = ::HIR::Literal( l / r );  break;
                        case ::MIR::eBinOp::MOD:    val = ::HIR::Literal( l % r );  break;
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            TODO(sp, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
                        
                        case ::MIR::eBinOp::BIT_OR : val = ::HIR::Literal( l | r );  break;
                        case ::MIR::eBinOp::BIT_AND: val = ::HIR::Literal( l & r );  break;
                        case ::MIR::eBinOp::BIT_XOR: val = ::HIR::Literal( l ^ r );  break;
                        case ::MIR::eBinOp::BIT_SHL: val = ::HIR::Literal( l << r );  break;
                        case ::MIR::eBinOp::BIT_SHR: val = ::HIR::Literal( l >> r );  break;
                        // TODO: GT/LT are incorrect for signed integers
                        case ::MIR::eBinOp::EQ: val = ::HIR::Literal( static_cast<uint64_t>(l == r) );  break;
                        case ::MIR::eBinOp::NE: val = ::HIR::Literal( static_cast<uint64_t>(l != r) );  break;
                        case ::MIR::eBinOp::GT: val = ::HIR::Literal( static_cast<uint64_t>(l >  r) );  break;
                        case ::MIR::eBinOp::GE: val = ::HIR::Literal( static_cast<uint64_t>(l >= r) );  break;
                        case ::MIR::eBinOp::LT: val = ::HIR::Literal( static_cast<uint64_t>(l <  r) );  break;
                        case ::MIR::eBinOp::LE: val = ::HIR::Literal( static_cast<uint64_t>(l <= r) );  break;
                        }
                        )
                    )
                    ),
                (UniOp,
                    auto inval = read_lval(e.val);
                    TU_IFLET( ::HIR::Literal, inval, Integer, i,
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            val = ::HIR::Literal( ~i );
                        break;
                        case ::MIR::eUniOp::NEG:
                            val = ::HIR::Literal( -i );
                            break;
                        }
                    )
                    else {
                        BUG(sp, "Invalid invert of " << inval.tag_str());
                    }
                    ),
                (DstMeta,
                    TODO(sp, "RValue::DstMeta");
                    ),
                (MakeDst,
                    TODO(sp, "RValue::MakeDst");
                    ),
                (Tuple,
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals)
                        vals.push_back( read_lval(v) );
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    ),
                (Array,
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals)
                        vals.push_back( read_lval(v) );
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    ),
                (Variant,
                    TODO(sp, "MIR _Variant");
                    ),
                (Struct,
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals)
                        vals.push_back( read_lval(v) );
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    )
                )
                
                auto& dst = get_lval(sa.dst);
                dst = mv$(val);
            }
            TU_MATCH_DEF( ::MIR::Terminator, (block.terminator), (e),
            (
                BUG(sp, "Unexpected terminator - " << block.terminator);
                ),
            (Goto,
                cur_block = e;
                ),
            (Return,
                return retval;
                ),
            (Call,
                auto& dst = get_lval(e.ret_val);
                auto fcn_v = read_lval(e.fcn_val);
                if( ! fcn_v.is_BorrowOf() ) {
                    BUG(sp, "Execute MIR - Calling function through invalid value - " << fcn_v);
                }
                const auto& fcnp = fcn_v.as_BorrowOf();
                auto& fcn = get_function(sp, crate, fcnp);
                
                ::std::vector< ::HIR::Literal>  call_args;
                call_args.reserve( e.args.size() );
                for(const auto& a : e.args)
                    call_args.push_back( read_lval(a) );
                // TODO: Set m_const during parse and check here
                
                // Call by invoking evaluate_constant on the function
                {
                    TRACE_FUNCTION_F("Call const fn " << fcnp << " args={ " << call_args << " }");
                    dst = evaluate_constant(sp, crate, newval_state,  fcn.m_code, mv$(call_args));
                }
                
                cur_block = e.ret_block;
                )
            )
        }
    }
    
    ::HIR::Literal evaluate_constant(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::HIR::ExprPtr& expr, ::std::vector< ::HIR::Literal> args)
    {
        if( expr ) {
            return evaluate_constant_hir(sp, crate, mv$(newval_state), *expr, mv$(args));
        }
        else if( expr.m_mir ) {
            return evaluate_constant_mir(sp, crate, mv$(newval_state), *expr.m_mir, mv$(args));
        }
        else {
            BUG(sp, "Attempting to evaluate constant expression with code");
        }
    }
    
    void check_lit_type(const Span& sp, const ::HIR::TypeRef& type,  ::HIR::Literal& lit)
    {
        // TODO: Mask down limited size integers
        TU_MATCHA( (type.m_data), (te),
        (Infer,
            ),
        (Diverge,
            ),
        (Generic,
            ),
        (Slice,
            ),
        (TraitObject,
            ),
        (ErasedType,
            ),
        (Closure,
            ),
        
        (Path,
            // List
            ),
        (Array,
            // List
            ),
        (Tuple,
            // List
            ),
        
        (Borrow,
            // A whole host of things
            ),
        (Pointer,
            // Integer, or itemaddr?
            ),
        (Function,
            // ItemAddr
            ),
        
        (Primitive,
            switch(te)
            {
            case ::HIR::CoreType::Str:
                BUG(sp, "Direct str literal not valid");
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                ASSERT_BUG(sp, lit.is_Float(), "Bad literal type for " << type << " - " << lit);
                break;
            default:
                ASSERT_BUG(sp, lit.is_Integer(), "Bad literal type for " << type << " - " << lit);
                switch(te)
                {
                case ::HIR::CoreType::U8:   lit.as_Integer() &= (1ull<<8)-1;  break;
                case ::HIR::CoreType::U16:  lit.as_Integer() &= (1ull<<16)-1; break;
                case ::HIR::CoreType::U32:  lit.as_Integer() &= (1ull<<32)-1; break;
                default:
                    break;
                }
                break;
            }
            )
        )
    }

    class Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        const ::HIR::ItemPath*  m_mod_path;
        t_new_values    m_new_values;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate)
        {}
        
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved_mp = m_mod_path;
            m_mod_path = &p;
            auto saved = mv$( m_new_values );
            
            ::HIR::Visitor::visit_module(p, mod);
            
            //auto items = mv$( m_new_values );
            //for( auto item : items )
            //{
            //    
            //}
            m_new_values = mv$(saved);
            m_mod_path = saved_mp;
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            ::HIR::Visitor::visit_type(ty);
            
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                if( e.size_val == ~0u )
                {
                    assert(e.size);
                    assert(*e.size);
                    const auto& expr_ptr = *e.size;
                    auto val = evaluate_constant(expr_ptr->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT("ty_" << &ty << "$") }, expr_ptr);
                    if( !val.is_Integer() )
                        ERROR(expr_ptr->span(), E0000, "Array size isn't an integer");
                    e.size_val = val.as_Integer();
                }
                DEBUG("Array " << ty << " - size = " << e.size_val);
            )
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            visit_type(item.m_type);
            if( item.m_value )
            {
                item.m_value_res = evaluate_constant(item.m_value->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$") }, item.m_value, {});
                
                check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);
                
                DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
                visit_expr(item.m_value);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            visit_type(item.m_type);
            if( item.m_value )
            {
                item.m_value_res = evaluate_constant(item.m_value->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$") }, item.m_value, {});
                DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
                visit_expr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    e.val = evaluate_constant(e.expr->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$" << var.first << "$") }, e.expr, {});
                    DEBUG("enum variant: " << p << "::" << var.first << " = " << e.val);
                )
            }
            ::HIR::Visitor::visit_enum(p, item);
        }
        
        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct Visitor:
                public ::HIR::ExprVisitorDef
            {
                Expander& m_exp;
                
                Visitor(Expander& exp):
                    m_exp(exp)
                {}
                
                void visit(::HIR::ExprNode_Let& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_type);
                }
                void visit(::HIR::ExprNode_Cast& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_res_type);
                }
                // TODO: This shouldn't exist yet?
                void visit(::HIR::ExprNode_Unsize& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_res_type);
                }
                void visit(::HIR::ExprNode_Closure& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_return);
                    for(auto& a : node.m_args)
                        m_exp.visit_type(a.second);
                }

                void visit(::HIR::ExprNode_ArraySized& node) override {
                    assert( node.m_size );
                    auto val = evaluate_constant_hir(node.span(), m_exp.m_crate, NewvalState { m_exp.m_new_values, *m_exp.m_mod_path, FMT("array_" << &node << "$") }, *node.m_size, {});
                    if( !val.is_Integer() )
                        ERROR(node.span(), E0000, "Array size isn't an integer");
                    node.m_size_val = val.as_Integer();
                    DEBUG("Array literal [?; " << node.m_size_val << "]");
                }
                
                void visit(::HIR::ExprNode_CallPath& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                }
                void visit(::HIR::ExprNode_CallMethod& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_path_params(node.m_params);
                }
            };
            
            if( expr.get() != nullptr )
            {
                Visitor v { *this };
                (*expr).visit(v);
            }
        }
    };
}   // namespace

void ConvertHIR_ConstantEvaluate(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}
