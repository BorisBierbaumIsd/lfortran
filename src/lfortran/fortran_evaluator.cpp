#include <iostream>
#include <fstream>

#include <lfortran/fortran_evaluator.h>
#include <libasr/codegen/asr_to_cpp.h>
#include <libasr/codegen/asr_to_c.h>
#include <libasr/codegen/asr_to_wasm.h>
#include <libasr/codegen/asr_to_julia.h>
#include <libasr/codegen/wasm_to_wat.h>
#include <lfortran/ast_to_src.h>
#include <libasr/exception.h>
#include <lfortran/ast.h>
#include <libasr/asr.h>
#include <lfortran/semantics/ast_to_asr.h>
#include <lfortran/parser/parser.h>
#include <lfortran/parser/preprocessor.h>
#include <lfortran/pickle.h>
#include <libasr/utils.h>

#ifdef HAVE_LFORTRAN_LLVM
#include <libasr/codegen/evaluator.h>
#include <libasr/codegen/asr_to_llvm.h>
#else
namespace LCompilers {
    class LLVMEvaluator {};
}
#endif

namespace LCompilers {


/* ------------------------------------------------------------------------- */
// FortranEvaluator

FortranEvaluator::FortranEvaluator(CompilerOptions compiler_options)
    :
    al{1024*1024},
#ifdef HAVE_LFORTRAN_LLVM
    e{std::make_unique<LLVMEvaluator>()},
    eval_count{0},
#endif
    compiler_options{compiler_options},
    symbol_table{nullptr}
{
}

FortranEvaluator::~FortranEvaluator() = default;

Result<FortranEvaluator::EvalResult> FortranEvaluator::evaluate2(const std::string &code) {
    LocationManager lm;
    LCompilers::PassManager lpm;
    lpm.use_default_passes();
    lpm.do_not_use_optimization_passes();
    {
        LocationManager::FileLocations fl;
        fl.in_filename = "input";
        std::ofstream out("input");
        out << code;
        lm.files.push_back(fl);
    }
    diag::Diagnostics diagnostics;
    return evaluate(code, false, lm, lpm, diagnostics);
}

Result<FortranEvaluator::EvalResult> FortranEvaluator::evaluate(
#ifdef HAVE_LFORTRAN_LLVM
            const std::string &code_orig, bool verbose, LocationManager &lm,
            LCompilers::PassManager& pass_manager, diag::Diagnostics &diagnostics
#else
            const std::string &/*code_orig*/, bool /*verbose*/,
                LocationManager &/*lm*/, LCompilers::PassManager& /*pass_manager*/,
                diag::Diagnostics &/*diagnostics*/
#endif
            )
{
#ifdef HAVE_LFORTRAN_LLVM
    EvalResult result;

    // Src -> AST
    Result<LFortran::AST::TranslationUnit_t*> res = get_ast2(
        code_orig, lm, diagnostics);
    LFortran::AST::TranslationUnit_t* ast;
    if (res.ok) {
        ast = res.result;
    } else {
        return res.error;
    }

    if (verbose) {
        result.ast = LFortran::pickle(*ast, true);
    }

    // AST -> ASR
    Result<ASR::TranslationUnit_t*> res2 = get_asr3(*ast, diagnostics);
    ASR::TranslationUnit_t* asr;
    if (res2.ok) {
        asr = res2.result;
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res2.error;
    }

    if (verbose) {
        result.asr = LFortran::pickle(*asr, true);
    }

    // ASR -> LLVM
    Result<std::unique_ptr<LLVMModule>> res3 = get_llvm3(*asr,
        pass_manager, diagnostics, lm.files.back().in_filename);
    std::unique_ptr<LCompilers::LLVMModule> m;
    if (res3.ok) {
        m = std::move(res3.result);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res3.error;
    }

    if (verbose) {
        result.llvm_ir = m->str();
    }

    std::string return_type = m->get_return_type(run_fn);

    // LLVM -> Machine code -> Execution
    e->add_module(std::move(m));
    if (return_type == "integer4") {
        int32_t r = e->int32fn(run_fn);
        result.type = EvalResult::integer4;
        result.i32 = r;
    } else if (return_type == "integer8") {
        int64_t r = e->int64fn(run_fn);
        result.type = EvalResult::integer8;
        result.i64 = r;
    } else if (return_type == "real4") {
        float r = e->floatfn(run_fn);
        result.type = EvalResult::real4;
        result.f32 = r;
    } else if (return_type == "real8") {
        double r = e->doublefn(run_fn);
        result.type = EvalResult::real8;
        result.f64 = r;
    } else if (return_type == "complex4") {
        std::complex<float> r = e->complex4fn(run_fn);
        result.type = EvalResult::complex4;
        result.c32.re = r.real();
        result.c32.im = r.imag();
    } else if (return_type == "complex8") {
        std::complex<double> r = e->complex8fn(run_fn);
        result.type = EvalResult::complex8;
        result.c64.re = r.real();
        result.c64.im = r.imag();
    } else if (return_type == "void") {
        e->voidfn(run_fn);
        result.type = EvalResult::statement;
    } else if (return_type == "none") {
        result.type = EvalResult::none;
    } else {
        throw LCompilersException("FortranEvaluator::evaluate(): Return type not supported");
    }
    return result;
#else
    throw LCompilersException("LLVM is not enabled");
#endif
}

Result<std::string> FortranEvaluator::get_ast(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics)
{
    Result<LFortran::AST::TranslationUnit_t*> ast = get_ast2(code, lm,
        diagnostics);
    if (ast.ok) {
        if (compiler_options.tree) {
            return LFortran::pickle_tree(*ast.result, compiler_options.use_colors);
        } else if (compiler_options.json) {
            return LFortran::pickle_json(*ast.result, lm);
        }
        return LFortran::pickle(*ast.result, compiler_options.use_colors,
            compiler_options.indent);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return ast.error;
    }
}

Result<LFortran::AST::TranslationUnit_t*> FortranEvaluator::get_ast2(
            const std::string &code_orig, LocationManager &lm,
            diag::Diagnostics &diagnostics)
{
    // Src -> AST
    const std::string *code=&code_orig;
    std::string tmp;
    if (compiler_options.c_preprocessor) {
        // Preprocessor
        LFortran::CPreprocessor cpp(compiler_options);
        tmp = cpp.run(code_orig, lm, cpp.macro_definitions);
        code = &tmp;
    }
    if (compiler_options.prescan || compiler_options.fixed_form) {
        tmp = LFortran::prescan(*code, lm, compiler_options.fixed_form,
            parent_path(lm.files.back().in_filename));
        code = &tmp;
    }
    Result<LFortran::AST::TranslationUnit_t*>
        res = LFortran::parse(al, *code, diagnostics,
        compiler_options.fixed_form);
    if (res.ok) {
        return res.result;
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }
}

Result<std::string> FortranEvaluator::get_asr(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics)
{
    Result<ASR::TranslationUnit_t*> asr = get_asr2(code, lm, diagnostics);
    if (asr.ok) {
        if (compiler_options.tree) {
            return LFortran::pickle_tree(*asr.result, compiler_options.use_colors);
        } else if (compiler_options.json) {
            return LFortran::pickle_json(*asr.result, lm);
        }
        return LFortran::pickle(*asr.result,
            compiler_options.use_colors, compiler_options.indent);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return asr.error;
    }
}

Result<ASR::TranslationUnit_t*> FortranEvaluator::get_asr2(
            const std::string &code_orig, LocationManager &lm,
            diag::Diagnostics &diagnostics)
{
    // Src -> AST
    Result<LFortran::AST::TranslationUnit_t*>
        res = get_ast2(code_orig, lm, diagnostics);
    LFortran::AST::TranslationUnit_t* ast;
    if (res.ok) {
        ast = res.result;
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }

    // AST -> ASR
    Result<ASR::TranslationUnit_t*> res2 = get_asr3(*ast, diagnostics);
    if (res2.ok) {
        return res2.result;
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res2.error;
    }
}

Result<ASR::TranslationUnit_t*> FortranEvaluator::get_asr3(
    LFortran::AST::TranslationUnit_t &ast, diag::Diagnostics &diagnostics)
{
    ASR::TranslationUnit_t* asr;
    // AST -> ASR
    // Remove the old execution function if it exists
    if (symbol_table) {
        if (symbol_table->get_symbol(run_fn) != nullptr) {
            symbol_table->erase_symbol(run_fn);
        }
        symbol_table->mark_all_variables_external(al);
    }
    auto res = LFortran::ast_to_asr(al, ast, diagnostics, symbol_table,
        compiler_options.symtab_only, compiler_options);
    if (res.ok) {
        asr = res.result;
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }
    if (!symbol_table) symbol_table = asr->m_global_scope;

    return asr;
}

Result<std::string> FortranEvaluator::get_llvm(
    const std::string &code, LocationManager &lm, LCompilers::PassManager& pass_manager,
    diag::Diagnostics &diagnostics
    )
{
    Result<std::unique_ptr<LLVMModule>> res = get_llvm2(code, lm, pass_manager, diagnostics);
    if (res.ok) {
#ifdef HAVE_LFORTRAN_LLVM
        return res.result->str();
#else
        throw LCompilersException("LLVM is not enabled");
#endif
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }
}

Result<std::unique_ptr<LLVMModule>> FortranEvaluator::get_llvm2(
    const std::string &code, LocationManager &lm, LCompilers::PassManager& pass_manager,
    diag::Diagnostics &diagnostics)
{
    Result<ASR::TranslationUnit_t*> asr = get_asr2(code, lm, diagnostics);
    if (!asr.ok) {
        return asr.error;
    }
    Result<std::unique_ptr<LLVMModule>> res = get_llvm3(*asr.result, pass_manager,
        diagnostics, lm.files.back().in_filename);
    if (res.ok) {
#ifdef HAVE_LFORTRAN_LLVM
        std::unique_ptr<LLVMModule> m = std::move(res.result);
        return m;
#else
        throw LCompilersException("LLVM is not enabled");
#endif
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }
}

Result<std::unique_ptr<LLVMModule>> FortranEvaluator::get_llvm3(
#ifdef HAVE_LFORTRAN_LLVM
    ASR::TranslationUnit_t &asr, LCompilers::PassManager& pass_manager,
    diag::Diagnostics &diagnostics
#else
    ASR::TranslationUnit_t &/*asr*/, LCompilers::PassManager &/*pass_manager*/,
    diag::Diagnostics &/*diagnostics*/
#endif
, const std::string &infile)
{
#ifdef HAVE_LFORTRAN_LLVM
    eval_count++;
    run_fn = "__lfortran_evaluate_" + std::to_string(eval_count);

    // ASR -> LLVM
    std::unique_ptr<LCompilers::LLVMModule> m;
    Result<std::unique_ptr<LCompilers::LLVMModule>> res
        = asr_to_llvm(asr, diagnostics,
            e->get_context(), al, pass_manager,
            compiler_options, run_fn, infile);
    if (res.ok) {
        m = std::move(res.result);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }

    if (compiler_options.fast) {
        e->opt(*m->m_m);
    }

    return m;
#else
    throw LCompilersException("LLVM is not enabled");
#endif
}

Result<std::string> FortranEvaluator::get_asm(
#ifdef HAVE_LFORTRAN_LLVM
    const std::string &code, LocationManager &lm,
    LCompilers::PassManager& lpm,
    diag::Diagnostics &diagnostics
#else
    const std::string &/*code*/,
    LocationManager &/*lm*/,
    LCompilers::PassManager&/*lpm*/,
    diag::Diagnostics &/*diagnostics*/
#endif
    )
{
#ifdef HAVE_LFORTRAN_LLVM
    Result<std::unique_ptr<LLVMModule>> res = get_llvm2(code, lm, lpm, diagnostics);
    if (res.ok) {
        return e->get_asm(*res.result->m_m);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return res.error;
    }
#else
    throw LCompilersException("LLVM is not enabled");
#endif
}

Result<Vec<uint8_t>> FortranEvaluator::get_wasm(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics)
{
    // Src -> AST -> ASR -> WASM
    SymbolTable *old_symbol_table = symbol_table;
    symbol_table = nullptr;
    Result<ASR::TranslationUnit_t*> asr = get_asr2(code, lm, diagnostics);
    symbol_table = old_symbol_table;
    if (asr.ok) {
        return asr_to_wasm_bytes_stream(*asr.result, al, diagnostics);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return asr.error;
    }
}

Result<std::string> FortranEvaluator::get_wat(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics)
{
    // Src -> AST -> ASR -> WASM -> WAT
    SymbolTable *old_symbol_table = symbol_table;
    symbol_table = nullptr;
    Result<Vec<uint8_t>> wasm = get_wasm(code, lm, diagnostics);
    symbol_table = old_symbol_table;
    if (wasm.ok) {
            return wasm_to_wat(wasm.result, al, diagnostics);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return wasm.error;
    }
}

Result<std::string> FortranEvaluator::get_cpp(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics, int64_t default_lower_bound)
{
    // Src -> AST -> ASR
    SymbolTable *old_symbol_table = symbol_table;
    symbol_table = nullptr;
    Result<ASR::TranslationUnit_t*> asr = get_asr2(code, lm, diagnostics);
    symbol_table = old_symbol_table;
    if (asr.ok) {
        return get_cpp2(*asr.result, diagnostics, default_lower_bound);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return asr.error;
    }
}

Result<std::string> FortranEvaluator::get_cpp2(ASR::TranslationUnit_t &asr,
        diag::Diagnostics &diagnostics, int64_t default_lower_bound)
{
    // ASR -> C++
    return asr_to_cpp(al, asr, diagnostics, compiler_options,
                      default_lower_bound);
}

Result<std::string> FortranEvaluator::get_c(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics,
    int64_t default_lower_bound)
{
    // Src -> AST -> ASR
    SymbolTable *old_symbol_table = symbol_table;
    symbol_table = nullptr;
    Result<ASR::TranslationUnit_t*> asr = get_asr2(code, lm, diagnostics);
    symbol_table = old_symbol_table;
    if (asr.ok) {
        return get_c2(*asr.result, diagnostics, default_lower_bound);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return asr.error;
    }
}

Result<std::string> FortranEvaluator::get_c2(ASR::TranslationUnit_t &asr,
        diag::Diagnostics &diagnostics, int64_t default_lower_bound)
{
    // ASR -> C++
    return asr_to_c(al, asr, diagnostics, compiler_options,
                    default_lower_bound);
}

Result<std::string> FortranEvaluator::get_julia(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics)
{
    // Src -> AST -> ASR -> Julia
    SymbolTable *old_symbol_table = symbol_table;
    symbol_table = nullptr;
    Result<ASR::TranslationUnit_t*> asr = get_asr2(code, lm, diagnostics);
    symbol_table = old_symbol_table;
    if (asr.ok) {
        return asr_to_julia(al, *asr.result, diagnostics);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return asr.error;
    }
}

Result<std::string> FortranEvaluator::get_fmt(const std::string &code,
    LocationManager &lm, diag::Diagnostics &diagnostics)
{
    // Src -> AST
    Result<LFortran::AST::TranslationUnit_t*> ast = get_ast2(code, lm, diagnostics);
    if (ast.ok) {
        // AST -> Fortran
        return LFortran::ast_to_src(*ast.result, true);
    } else {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return ast.error;
    }
}

} // namespace LCompilers
