// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "module.h"

#include <assert.h>
#include <stdio.h>

#include <algorithm>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

namespace fidl {

namespace {

const TypeShape kHandleTypeShape = TypeShape(4u, 4u);
const TypeShape kInt8TypeShape = TypeShape(1u, 1u);
const TypeShape kInt16TypeShape = TypeShape(2u, 2u);
const TypeShape kInt32TypeShape = TypeShape(4u, 4u);
const TypeShape kInt64TypeShape = TypeShape(8u, 8u);
const TypeShape kUint8TypeShape = TypeShape(1u, 1u);
const TypeShape kUint16TypeShape = TypeShape(2u, 2u);
const TypeShape kUint32TypeShape = TypeShape(4u, 4u);
const TypeShape kUint64TypeShape = TypeShape(8u, 8u);
const TypeShape kBoolTypeShape = TypeShape(1u, 1u);
const TypeShape kFloat32TypeShape = TypeShape(4u, 4u);
const TypeShape kFloat64TypeShape = TypeShape(8u, 8u);
const TypeShape kPointerTypeShape = TypeShape(8u, 8u);

size_t AlignTo(size_t size, size_t alignment) {
    auto mask = alignment - 1;
    size += mask;
    size &= ~mask;
    return size;
}

TypeShape StructTypeShape(std::vector<TypeShape> member_typeshapes) {
    // TODO(kulakowski) Stable sort struct members by size/alignment.

    size_t size = 0u;
    size_t alignment = 1u;

    for (const auto& type_shape : member_typeshapes) {
        alignment = std::max(alignment, type_shape.Alignment());
        size = AlignTo(size, type_shape.Alignment());
        size += type_shape.Size();
    }

    return TypeShape(size, alignment);
}

TypeShape UnionTypeShape(std::vector<TypeShape> member_typeshapes) {
    size_t size = 0u;
    size_t alignment = 1u;
    for (const auto& type_shape : member_typeshapes) {
        size = std::max(size, type_shape.Size());
        alignment = std::max(alignment, type_shape.Alignment());
    }
    size = AlignTo(size, alignment);
    return TypeShape(size, alignment);
}

TypeShape ArrayTypeShape(TypeShape element, uint64_t count) {
    return TypeShape(element.Size() * count, element.Alignment());
}

TypeShape VectorTypeShape(TypeShape element, uint64_t count) {
    auto array_allocation = Allocation(ArrayTypeShape(element, count));
    auto header_shape = StructTypeShape(std::vector<TypeShape>({kUint64TypeShape, kPointerTypeShape}));
    header_shape.AddAllocation(array_allocation);
    return header_shape;
}

TypeShape StringTypeShape(uint64_t count) {
    auto array_allocation = Allocation(ArrayTypeShape(kInt8TypeShape, count));
    auto header_shape = StructTypeShape(std::vector<TypeShape>({kUint64TypeShape, kPointerTypeShape}));
    header_shape.AddAllocation(array_allocation);
    return header_shape;
}

template <typename IntType>
bool ParseIntegerLiteral(const NumericLiteral* literal, IntType* out_value) {
    if (!literal)
        return false;
    auto data = literal->literal.data();
    std::string string_data(data.data(), data.data() + data.size());
    if (std::is_unsigned<IntType>::value) {
        errno = 0;
        unsigned long long value = strtoull(string_data.data(), nullptr, 0);
        if (errno != 0)
            return false;
        if (value > std::numeric_limits<IntType>::max())
            return false;
        *out_value = static_cast<IntType>(value);
    } else {
        errno = 0;
        long long value = strtoll(string_data.data(), nullptr, 0);
        if (errno != 0)
            return false;
        if (value > std::numeric_limits<IntType>::max())
            return false;
        if (value < std::numeric_limits<IntType>::min())
            return false;
        *out_value = static_cast<IntType>(value);
    }
    return true;
}

template <typename IntType>
bool ParseIntegerConstant(const Constant* constant, IntType* out_value) {
    if (!constant)
        return false;
    switch (constant->kind) {
    case Constant::Kind::Identifier: {
        auto identifier_constant = static_cast<const IdentifierConstant*>(constant);
        auto identifier = identifier_constant->identifier.get();
        // TODO(kulakowski) Actually resolve this.
        static_cast<void>(identifier);
        *out_value = static_cast<IntType>(23);
        return true;
    }
    case Constant::Kind::Literal: {
        auto literal_constant = static_cast<const LiteralConstant*>(constant);
        switch (literal_constant->literal->kind) {
        case Literal::Kind::String:
        case Literal::Kind::True:
        case Literal::Kind::False:
        case Literal::Kind::Default:
            return false;

        case Literal::Kind::Numeric: {
            auto numeric_literal = static_cast<const NumericLiteral*>(literal_constant->literal.get());
            return ParseIntegerLiteral<IntType>(numeric_literal, out_value);
        }
        }
    }
    }
}

} // namespace

bool Module::Parse(StringView source) {
    Lexer lexer(source, &identifier_table_);
    Parser parser(&lexer);
    auto ast = parser.Parse();
    if (!parser.Ok())
        return false;
    assert(ast != nullptr);
    if (!ConsumeFile(std::move(ast)))
        return false;
    // After consumption, we have flattened the representation, but we
    // haven't resolved references yet. Do so now.
    return Resolve();
}

// Consuming the AST is primarily concerned with walking the tree and
// flattening the representation. The AST's declaration nodes are
// converted into the Module's foo_info structures.

bool Module::ConsumeConstDeclaration(std::unique_ptr<ConstDeclaration> const_declaration) {
    auto name = Name(std::move(const_declaration->identifier));

    if (!RegisterTypeName(name))
        return false;
    const_infos_.emplace_back(std::move(name),
                              std::move(const_declaration->type),
                              std::move(const_declaration->constant));
    return true;
}

bool Module::ConsumeEnumDeclaration(std::unique_ptr<EnumDeclaration> enum_declaration) {
    std::vector<EnumInfo::Member> members;
    for (auto& member : enum_declaration->members) {
        auto name = Name(std::move(member->identifier));
        // TODO(kulakowski) enum values
        members.emplace_back(std::move(name), nullptr);
    }
    std::unique_ptr<PrimitiveType> type = std::move(enum_declaration->maybe_subtype);
    if (!type)
        type = std::make_unique<PrimitiveType>(PrimitiveType::Subtype::Uint32);
    auto name = Name(std::move(enum_declaration->identifier));

    if (!RegisterTypeName(name))
        return false;
    enum_infos_.emplace_back(std::move(name), std::move(type), std::move(members));
    return true;
}

bool Module::ConsumeInterfaceDeclaration(std::unique_ptr<InterfaceDeclaration> interface_declaration) {
    auto name = Name(std::move(interface_declaration->identifier));

    for (auto& const_member : interface_declaration->const_members)
        if (!ConsumeConstDeclaration(std::move(const_member)))
            return false;
    for (auto& enum_member : interface_declaration->enum_members)
        if (!ConsumeEnumDeclaration(std::move(enum_member)))
            return false;

    std::vector<InterfaceInfo::Method> methods;
    for (auto& method : interface_declaration->method_members) {
        auto ordinal_literal = std::move(method->ordinal);
        uint32_t value;
        if (!ParseIntegerLiteral<decltype(value)>(ordinal_literal.get(), &value))
            return false;
        Ordinal ordinal(std::move(ordinal_literal), value);

        auto method_name = Name(std::move(method->identifier));

        std::vector<InterfaceInfo::Method::Parameter> parameters;
        for (auto& parameter : method->parameter_list->parameter_list) {
            auto parameter_name = Name(std::move(parameter->identifier));
            parameters.emplace_back(std::move(parameter->type),
                                    std::move(parameter_name));
        }

        bool has_response = static_cast<bool>(method->maybe_response);

        std::vector<InterfaceInfo::Method::Parameter> maybe_response;
        if (has_response) {
            for (auto& parameter : method->maybe_response->parameter_list) {
                auto response_paramater_name = Name(std::move(parameter->identifier));
                maybe_response.emplace_back(std::move(parameter->type),
                                            std::move(response_paramater_name));
            }
        }

        methods.emplace_back(
            std::move(ordinal),
            std::move(method_name),
            std::move(parameters),
            has_response,
            std::move(maybe_response));
    }

    if (!RegisterTypeName(name))
        return false;
    interface_infos_.emplace_back(std::move(name), std::move(methods));
    return true;
}

bool Module::ConsumeStructDeclaration(std::unique_ptr<StructDeclaration> struct_declaration) {
    auto name = Name(std::move(struct_declaration->identifier));

    for (auto& const_member : struct_declaration->const_members)
        if (!ConsumeConstDeclaration(std::move(const_member)))
            return false;
    for (auto& enum_member : struct_declaration->enum_members)
        if (!ConsumeEnumDeclaration(std::move(enum_member)))
            return false;

    std::vector<StructInfo::Member> members;
    for (auto& member : struct_declaration->members) {
        auto name = Name(std::move(member->identifier));
        members.emplace_back(
            std::move(member->type),
            std::move(name),
            std::move(member->maybe_default_value));
    }

    if (!RegisterTypeName(name))
        return false;
    struct_infos_.emplace_back(std::move(name), std::move(members));
    return true;
}

bool Module::ConsumeUnionDeclaration(std::unique_ptr<UnionDeclaration> union_declaration) {
    std::vector<UnionInfo::Member> members;
    for (auto& member : union_declaration->members) {
        auto name = Name(std::move(member->identifier));
        members.emplace_back(std::move(member->type), std::move(name));
    }
    auto name = Name(std::move(union_declaration->identifier));

    if (!RegisterTypeName(name))
        return false;
    union_infos_.emplace_back(std::move(name), std::move(members));
    return true;
}

bool Module::ConsumeFile(std::unique_ptr<File> file) {
    auto module_name = std::move(file->identifier);

    auto using_list = std::move(file->using_list);

    auto const_declaration_list = std::move(file->const_declaration_list);
    for (auto& const_declaration : const_declaration_list)
        if (!ConsumeConstDeclaration(std::move(const_declaration)))
            return false;

    auto enum_declaration_list = std::move(file->enum_declaration_list);
    for (auto& enum_declaration : enum_declaration_list)
        if (!ConsumeEnumDeclaration(std::move(enum_declaration)))
            return false;

    auto interface_declaration_list = std::move(file->interface_declaration_list);
    for (auto& interface_declaration : interface_declaration_list)
        if (!ConsumeInterfaceDeclaration(std::move(interface_declaration)))
            return false;

    auto struct_declaration_list = std::move(file->struct_declaration_list);
    for (auto& struct_declaration : struct_declaration_list)
        if (!ConsumeStructDeclaration(std::move(struct_declaration)))
            return false;

    auto union_declaration_list = std::move(file->union_declaration_list);
    for (auto& union_declaration : union_declaration_list)
        if (!ConsumeUnionDeclaration(std::move(union_declaration)))
            return false;

    return true;
}

bool Module::RegisterTypeName(const Name& name) {
    auto iter = registered_types_.insert(name.data());
    return iter.second;
}

bool Module::RegisterResolvedType(const Name& name, TypeShape typeshape) {
    auto key_value = std::make_pair(name.data(), typeshape);
    auto iter = resolved_types_.insert(std::move(key_value));
    return iter.second;
}

// Module resolution is concerned with resolving identifiers to their
// declarations, and with computing type sizes and alignments.

bool Module::ResolveConstInfo(const ConstInfo& const_info) {
    if (!ResolveType(const_info.type.get()))
        return false;
    // TODO(kulakowski) Resolve const declarations.
    return true;
}

bool Module::ResolveEnumInfo(const EnumInfo& enum_info) {
    TypeShape typeshape;

    switch (enum_info.type->subtype) {
    case PrimitiveType::Subtype::Int8:
    case PrimitiveType::Subtype::Int16:
    case PrimitiveType::Subtype::Int32:
    case PrimitiveType::Subtype::Int64:
    case PrimitiveType::Subtype::Uint8:
    case PrimitiveType::Subtype::Uint16:
    case PrimitiveType::Subtype::Uint32:
    case PrimitiveType::Subtype::Uint64:
        // These are allowed as enum subtypes. Resolve the size and alignment.
        if (!ResolveType(enum_info.type.get(), &typeshape))
            return false;
        break;

    case PrimitiveType::Subtype::Bool:
    case PrimitiveType::Subtype::Float32:
    case PrimitiveType::Subtype::Float64:
        // These are not allowed as enum subtypes.
        return false;
    }

    if (!RegisterResolvedType(enum_info.name, typeshape))
        return false;

    // TODO(kulakowski) Validate values.
    return true;
}

bool Module::ResolveInterfaceInfo(const InterfaceInfo& interface_info) {
    Scope<StringView> name_scope;
    Scope<uint32_t> ordinal_scope;
    for (const auto& method : interface_info.methods) {
        if (!name_scope.Insert(method.name.data()))
            return false;
        if (!ordinal_scope.Insert(method.ordinal.Value()))
            return false;
        Scope<StringView> parameter_scope;
        for (const auto& param : method.parameter_list) {
            if (!parameter_scope.Insert(param.name.data()))
                return false;
            if (!ResolveType(param.type.get()))
                return false;
        }
        if (method.has_response) {
            Scope<StringView> response_scope;
            for (const auto& response_param : method.maybe_response) {
                if (!response_scope.Insert(response_param.name.data()))
                    return false;
                if (!ResolveType(response_param.type.get()))
                    return false;
            }
        }
    }
    return true;
}

bool Module::ResolveStructInfo(const StructInfo& struct_info) {
    Scope<StringView> scope;
    std::vector<TypeShape> member_typeshapes;
    for (const auto& member : struct_info.members) {
        if (!scope.Insert(member.name.data()))
            return false;
        TypeShape member_typeshape;
        if (!ResolveType(member.type.get(), &member_typeshape))
            return false;
        member_typeshapes.push_back(member_typeshape);
    }

    auto type_shape = StructTypeShape(std::move(member_typeshapes));
    if (!RegisterResolvedType(struct_info.name, type_shape))
        return false;

    return true;
}

bool Module::ResolveUnionInfo(const UnionInfo& union_info) {
    Scope<StringView> scope;
    std::vector<TypeShape> member_typeshapes;
    for (const auto& member : union_info.members) {
        if (!scope.Insert(member.name.data()))
            return false;
        TypeShape member_typeshape;
        if (!ResolveType(member.type.get(), &member_typeshape))
            return false;
        member_typeshapes.push_back(member_typeshape);
    }

    auto typeshape = UnionTypeShape(std::move(member_typeshapes));
    if (!RegisterResolvedType(union_info.name, typeshape))
        return false;

    return true;
}

bool Module::Resolve() {
    for (const auto& const_info : const_infos_)
        if (!ResolveConstInfo(const_info))
            return false;

    for (const auto& enum_info : enum_infos_)
        if (!ResolveEnumInfo(enum_info))
            return false;

    for (const auto& interface_info : interface_infos_)
        if (!ResolveInterfaceInfo(interface_info))
            return false;

    for (const auto& struct_info : struct_infos_)
        if (!ResolveStructInfo(struct_info))
            return false;

    for (const auto& union_info : union_infos_)
        if (!ResolveUnionInfo(union_info))
            return false;

    return true;
}

bool Module::ResolveArrayType(const ArrayType& array_type, TypeShape* out_typeshape) {
    TypeShape element_typeshape;
    if (!ResolveType(array_type.element_type.get(), &element_typeshape))
        return false;
    uint64_t element_count;
    if (!ParseIntegerConstant<decltype(element_count)>(array_type.element_count.get(),
                                                       &element_count))
        return false;
    if (element_count == 0)
        return false;
    *out_typeshape = ArrayTypeShape(element_typeshape, element_count);
    return true;
}

bool Module::ResolveVectorType(const VectorType& vector_type, TypeShape* out_typeshape) {
    TypeShape element_typeshape;
    if (!ResolveType(vector_type.element_type.get(), &element_typeshape))
        return false;
    auto element_count = std::numeric_limits<uint64_t>::max();
    if (vector_type.maybe_element_count) {
        if (!ParseIntegerConstant(vector_type.maybe_element_count.get(), &element_count))
            return false;
        if (element_count == 0u)
            return false;
    }
    *out_typeshape = VectorTypeShape(element_typeshape, element_count);
    return true;
}

bool Module::ResolveStringType(const StringType& string_type, TypeShape* out_typeshape) {
    auto byte_count = std::numeric_limits<uint64_t>::max();
    if (string_type.maybe_element_count) {
        if (!ParseIntegerConstant(string_type.maybe_element_count.get(), &byte_count))
            return false;
        if (byte_count == 0u)
            return false;
    }
    *out_typeshape = StringTypeShape(byte_count);
    return true;
}

bool Module::ResolveHandleType(const HandleType& handle_type, TypeShape* out_typeshape) {
    // Nothing to check.
    *out_typeshape = kHandleTypeShape;
    return true;
}

bool Module::ResolveRequestType(const RequestType& request_type, TypeShape* out_typeshape) {
    if (!ResolveTypeName(request_type.subtype.get()))
        return false;
    *out_typeshape = kHandleTypeShape;
    return true;
}

bool Module::ResolvePrimitiveType(const PrimitiveType& primitive_type, TypeShape* out_typeshape) {
    switch (primitive_type.subtype) {
    case PrimitiveType::Subtype::Int8:
        *out_typeshape = kInt8TypeShape;
        break;
    case PrimitiveType::Subtype::Int16:
        *out_typeshape = kInt16TypeShape;
        break;
    case PrimitiveType::Subtype::Int32:
        *out_typeshape = kInt32TypeShape;
        break;
    case PrimitiveType::Subtype::Int64:
        *out_typeshape = kInt64TypeShape;
        break;
    case PrimitiveType::Subtype::Uint8:
        *out_typeshape = kUint8TypeShape;
        break;
    case PrimitiveType::Subtype::Uint16:
        *out_typeshape = kUint16TypeShape;
        break;
    case PrimitiveType::Subtype::Uint32:
        *out_typeshape = kUint32TypeShape;
        break;
    case PrimitiveType::Subtype::Uint64:
        *out_typeshape = kUint64TypeShape;
        break;
    case PrimitiveType::Subtype::Bool:
        *out_typeshape = kBoolTypeShape;
        break;
    case PrimitiveType::Subtype::Float32:
        *out_typeshape = kFloat32TypeShape;
        break;
    case PrimitiveType::Subtype::Float64:
        *out_typeshape = kFloat64TypeShape;
        break;
    }
    return true;
}

bool Module::ResolveIdentifierType(const IdentifierType& identifier_type, TypeShape* out_typeshape) {
    if (!ResolveTypeName(identifier_type.identifier.get()))
        return false;
    // TODO(kulakowski) identifier type shape
    return true;
}

bool Module::ResolveType(const Type* type, TypeShape* out_typeshape) {
    switch (type->kind) {
    case Type::Kind::Array: {
        auto array_type = static_cast<const ArrayType*>(type);
        return ResolveArrayType(*array_type, out_typeshape);
    }

    case Type::Kind::Vector: {
        auto vector_type = static_cast<const VectorType*>(type);
        return ResolveVectorType(*vector_type, out_typeshape);
    }

    case Type::Kind::String: {
        auto string_type = static_cast<const StringType*>(type);
        return ResolveStringType(*string_type, out_typeshape);
    }

    case Type::Kind::Handle: {
        auto handle_type = static_cast<const HandleType*>(type);
        return ResolveHandleType(*handle_type, out_typeshape);
    }

    case Type::Kind::Request: {
        auto request_type = static_cast<const RequestType*>(type);
        return ResolveRequestType(*request_type, out_typeshape);
    }

    case Type::Kind::Primitive: {
        auto primitive_type = static_cast<const PrimitiveType*>(type);
        return ResolvePrimitiveType(*primitive_type, out_typeshape);
    }

    case Type::Kind::Identifier: {
        auto identifier_type = static_cast<const IdentifierType*>(type);
        return ResolveIdentifierType(*identifier_type, out_typeshape);
    }
    }
}

bool Module::ResolveTypeName(const CompoundIdentifier* name) {
    // TODO(kulakowski)
    assert(name->components.size() == 1);
    StringView identifier = name->components[0]->identifier.data();
    return registered_types_.find(identifier) != registered_types_.end();
}

bool Module::Dump() {
    printf("\nconst %zu\n", const_infos_.size());
    for (const auto& const_info : const_infos_) {
        auto name = const_info.name.data();
        auto type = resolved_types_[name];
        printf("\t%.*s\n", static_cast<int>(name.size()), name.data());
        printf("\t\tsize: %zu\n", type.Size());
        printf("\t\talignment: %zu\n", type.Alignment());
    }

    printf("\nenum %zu\n", enum_infos_.size());
    for (const auto& enum_info : enum_infos_) {
        auto name = enum_info.name.data();
        auto type = resolved_types_[name];
        printf("\t%.*s\n", static_cast<int>(name.size()), name.data());
        printf("\t\tsize: %zu\n", type.Size());
        printf("\t\talignment: %zu\n", type.Alignment());
    }

    printf("\ninterface %zu\n", interface_infos_.size());
    for (const auto& interface_info : interface_infos_) {
        auto name = interface_info.name.data();
        auto type = resolved_types_[name];
        printf("\t%.*s\n", static_cast<int>(name.size()), name.data());
        printf("\t\tsize: %zu\n", type.Size());
        printf("\t\talignment: %zu\n", type.Alignment());
    }

    printf("\nstruct %zu\n", struct_infos_.size());
    for (const auto& struct_info : struct_infos_) {
        auto name = struct_info.name.data();
        auto type = resolved_types_[name];
        printf("\t%.*s\n", static_cast<int>(name.size()), name.data());
        printf("\t\tsize: %zu\n", type.Size());
        printf("\t\talignment: %zu\n", type.Alignment());
    }

    printf("\nunion %zu\n", union_infos_.size());
    for (const auto& union_info : union_infos_) {
        auto name = union_info.name.data();
        auto type = resolved_types_[name];
        printf("\t%.*s\n", static_cast<int>(name.size()), name.data());
        printf("\t\tsize: %zu\n", type.Size());
        printf("\t\talignment: %zu\n", type.Alignment());
    }

    return true;
}

} // namespace fidl
