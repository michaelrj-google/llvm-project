//===-- LLVMDialect.td - LLVM IR dialect definition --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVMIR_DIALECT
#define LLVMIR_DIALECT

include "mlir/IR/DialectBase.td"

def LLVM_Dialect : Dialect {
  let name = "llvm";
  let cppNamespace = "::mlir::LLVM";

  let hasConstantMaterializer = 1;
  let useDefaultAttributePrinterParser = 1;
  let hasRegionArgAttrVerify = 1;
  let hasRegionResultAttrVerify = 1;
  let hasOperationAttrVerify = 1;

  let discardableAttrs = (ins
    /// Attribute encoding size and type of GPU workgroup attributions.
    "WorkgroupAttributionAttr":$workgroup_attribution
  );

  let extraClassDeclaration = [{
    /// Name of the data layout attributes.
    static StringRef getDataLayoutAttrName() { return "llvm.data_layout"; }
    static StringRef getNoAliasScopesAttrName() { return "noalias_scopes"; }
    static StringRef getAliasScopesAttrName() { return "alias_scopes"; }
    static StringRef getAccessGroupsAttrName() { return "access_groups"; }
    static StringRef getIdentAttrName() { return "llvm.ident"; }
    static StringRef getModuleFlags() { return "llvm.module.flags"; }
    static StringRef getCommandlineAttrName() { return "llvm.commandline"; }

    /// Names of llvm parameter attributes.
    static StringRef getAlignAttrName() { return "llvm.align"; }
    static StringRef getAllocAlignAttrName() { return "llvm.allocalign"; }
    static StringRef getAllocatedPointerAttrName() { return "llvm.allocptr"; }
    static StringRef getByValAttrName() { return "llvm.byval"; }
    static StringRef getByRefAttrName() { return "llvm.byref"; }
    static StringRef getNoUndefAttrName() { return "llvm.noundef"; }
    static StringRef getDereferenceableAttrName() { return "llvm.dereferenceable"; }
    static StringRef getDereferenceableOrNullAttrName() { return "llvm.dereferenceable_or_null"; }
    static StringRef getElementTypeAttrName() { return "llvm.elementtype"; }
    static StringRef getInAllocaAttrName() { return "llvm.inalloca"; }
    static StringRef getInRegAttrName() { return "llvm.inreg"; }
    static StringRef getNestAttrName() { return "llvm.nest"; }
    static StringRef getNoAliasAttrName() { return "llvm.noalias"; }
    static StringRef getNoCaptureAttrName() { return "llvm.nocapture"; }
    static StringRef getNoFreeAttrName() { return "llvm.nofree"; }
    static StringRef getNonNullAttrName() { return "llvm.nonnull"; }
    static StringRef getPreallocatedAttrName() { return "llvm.preallocated"; }
    static StringRef getRangeAttrName() { return "llvm.range"; }
    static StringRef getReadonlyAttrName() { return "llvm.readonly"; }
    static StringRef getReturnedAttrName() { return "llvm.returned"; }
    static StringRef getSExtAttrName() { return "llvm.signext"; }
    static StringRef getStackAlignmentAttrName() { return "llvm.alignstack"; }
    static StringRef getStructRetAttrName() { return "llvm.sret"; }
    static StringRef getWriteOnlyAttrName() { return "llvm.writeonly"; }
    static StringRef getZExtAttrName() { return "llvm.zeroext"; }
    static StringRef getOpBundleSizesAttrName() { return "op_bundle_sizes"; }
    static StringRef getOpBundleTagsAttrName() { return "op_bundle_tags"; }
    // TODO Restrict the usage of this to parameter attributes once there is an
    // alternative way of modeling memory effects on FunctionOpInterface.
    /// Name of the attribute that will cause the creation of a readnone memory
    /// effect when lowering to the LLVMDialect.
    static StringRef getReadnoneAttrName() { return "llvm.readnone"; }

    /// Verifies if the given string is a well-formed data layout descriptor.
    /// Uses `reportError` to report errors.
    static LogicalResult verifyDataLayoutString(
        StringRef descr, llvm::function_ref<void (const Twine &)> reportError);

    /// Name of the target triple attribute.
    static StringRef getTargetTripleAttrName() { return "llvm.target_triple"; }

    /// Name of the C wrapper emission attribute.
    static StringRef getEmitCWrapperAttrName() {
      return "llvm.emit_c_interface";
    }

    /// Name of the module level assembly attribute.
    static StringRef getModuleLevelAsmAttrName() { return "llvm.module_asm"; }

    /// Name of the dependent libraries attribute.
    static StringRef getDependentLibrariesAttrName() {
      return "llvm.dependent_libraries";
    }

    /// Names of known llvm module flag keys.
    static StringRef getModuleFlagKeyCGProfileName() {
      return "CG Profile";
    }
    static StringRef getModuleFlagKeyProfileSummaryName() {
      return "ProfileSummary";
    }

    /// Returns `true` if the given type is compatible with the LLVM dialect.
    static bool isCompatibleType(Type);


    Type parseType(DialectAsmParser &p) const override;
    void printType(Type, DialectAsmPrinter &p) const override;

  private:
    /// Verifies a parameter attribute attached to a parameter of type
    /// paramType.
    LogicalResult verifyParameterAttribute(Operation *op,
                                           Type paramType,
                                           NamedAttribute paramAttr);

    /// Register all types.
    void registerTypes();

    /// A cache storing compatible LLVM types that have been verified. This
    /// can save us lots of verification time if there are many occurrences
    /// of some deeply-nested aggregate types in the program.
    ThreadLocalCache<DenseSet<Type>> compatibleTypes;

    /// Register the attributes of this dialect.
    void registerAttributes();
  }];
}

#endif  // LLVMIR_DIALECT
