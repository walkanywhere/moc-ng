/****************************************************************************
 * Copyright (C) 2012 Woboq UG (haftungsbeschraenkt)
 * Olivier Goffart <contact at woboq.com>
 * http://woboq.com/
 ****************************************************************************/


#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Tool.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Lex/LexDiagnostic.h>

#include <clang/Driver/Job.h>
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include <clang/Lex/Preprocessor.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <llvm/Support/Host.h>

#include <vector>
#include <iostream>

#include "mocastconsumer.h"
#include "generator.h"
#include "mocppcallbacks.h"
#include "embedded_includes.h"

struct MocOptions {
  bool NoInclude = false;
  std::vector<std::string> Includes;
  std::string Output;
} Options;


struct MocDiagConsumer : clang::DiagnosticConsumer {
    llvm::OwningPtr<DiagnosticConsumer> Proxy;
    MocDiagConsumer(DiagnosticConsumer *Previous) : Proxy(Previous)  {}

    int HadRealError = 0;

    DiagnosticConsumer* clone(clang::DiagnosticsEngine& Diags) const override {
        return new MocDiagConsumer { Proxy->clone(Diags) };
    }
    void BeginSourceFile(const clang::LangOptions& LangOpts, const clang::Preprocessor* PP = 0) override {
        Proxy->BeginSourceFile(LangOpts, PP);
    }
    void clear() override {
        Proxy->clear();
    }
    void EndSourceFile() override {
        Proxy->EndSourceFile();
    }
    void finish() override {
        Proxy->finish();
    }
    void HandleDiagnostic(clang::DiagnosticsEngine::Level DiagLevel, const clang::Diagnostic& Info) override {

        auto DiagId = Info.getID();
        auto Cat = Info.getDiags()->getDiagnosticIDs()->getCategoryNumberForDiag(DiagId);

        bool ShouldReset = false;

//        std::cerr << "Category : " << Cat << std::endl;
        if (DiagLevel >= clang::DiagnosticsEngine::Error ) {
            if (Cat == 2 || Cat == 4
                || DiagId == clang::diag::err_param_redefinition
                || DiagId == clang::diag::err_pp_expr_bad_token_binop ) {
                if (!HadRealError)
                    ShouldReset = true;
                DiagLevel = clang::DiagnosticsEngine::Warning;
            } else {
                HadRealError++;
            }
        }

        DiagnosticConsumer::HandleDiagnostic(DiagLevel, Info);
        Proxy->HandleDiagnostic(DiagLevel, Info);

        if (ShouldReset) {
            // FIXME:  is there another way to ignore errors?
            const_cast<clang::DiagnosticsEngine *>(Info.getDiags())->Reset();
        }
    }
};



struct MocNGASTConsumer : public MocASTConsumer {
    std::string InFile;
    MocNGASTConsumer(clang::CompilerInstance& ci, llvm::StringRef InFile) : MocASTConsumer(ci), InFile(InFile) { }

    void Initialize(clang::ASTContext& Ctx) override {
        MocASTConsumer::Initialize(Ctx);

        if (llvm::StringRef(InFile).endswith("global/qnamespace.h")) {
            std::cerr << std::string(InFile) << std::endl;
            clang::Preprocessor &PP = ci.getPreprocessor();
            clang::MacroInfo *MI = PP.AllocateMacroInfo({});
            MI->setIsBuiltinMacro();
#if CLANG_VERSION_MAJOR != 3 || CLANG_VERSION_MINOR > 2
            PP.appendDefMacroDirective(PP.getIdentifierInfo("Q_MOC_RUN"), MI);
#else
            PP.setMacroInfo(PP.getIdentifierInfo("Q_MOC_RUN"), MI);
#endif
            PPCallbacks->InjectQObjectDefs({});
        }

    }

    void HandleTagDeclDefinition(clang::TagDecl* D) override {
        auto SL = D->getSourceRange().getBegin();
        SL = ci.getSourceManager().getExpansionLoc(SL);
        if (ci.getSourceManager().getFileID(SL) != ci.getSourceManager().getMainFileID())
            return;
        MocASTConsumer::HandleTagDeclDefinition(D);
    }

    void HandleTranslationUnit(clang::ASTContext& Ctx) override {

        if (ci.getDiagnostics().hasErrorOccurred())
            return;

        if (!objects.size()) {
          ci.getDiagnostics().Report(ci.getSourceManager().getLocForStartOfFile(ci.getSourceManager().getMainFileID()),
                                     ci.getDiagnostics().getCustomDiagID(clang::DiagnosticsEngine::Warning,
                                                                         "No relevant classes found. No output generated"));
          //actually still create an empty file like moc does.
          ci.createOutputFile(Options.Output, false);
          return;
        }

        llvm::raw_ostream *OS = ci.createOutputFile(Options.Output, false);

        if (!OS) return;
        llvm::raw_ostream &Out = *OS;

        Out <<  "/****************************************************************************\n"
                "** Meta object code from reading C++ file '" << InFile << "'\n"
                "**\n"
                "** Created by MOC-NG version " MOCNG_VERSION_STR " by Woboq [http://woboq.com]\n"
                "** WARNING! All changes made in this file will be lost!\n"
                "*****************************************************************************/\n\n";

        if (!Options.NoInclude) {
          for (auto &s : Options.Includes) {
            Out << s << "\n";
          }
          if (llvm::StringRef(InFile).endswith(".h"))
            Out << "#include \"" << InFile << "\"\n";
          if (Moc.HasPlugin)
            Out << "#include <QtCore/qplugin.h>\n";
        }

        Out << "#if !defined(Q_MOC_OUTPUT_REVISION)\n"
               "#error \"The header file '" << InFile << "' doesn't include <QObject>.\"\n"
               "#elif Q_MOC_OUTPUT_REVISION != " << mocOutputRevision << "\n"
               "#error \"This file was generated using MOC-NG " MOCNG_VERSION_STR ".\"\n"
               "#error \"It cannot be used with the include files from this version of Qt.\"\n"
               "#endif\n\n"
               "QT_BEGIN_MOC_NAMESPACE\n";


        for (const ClassDef &Def : objects ) {
          Generator G(&Def, Out, Ctx, &Moc);
          if (llvm::StringRef(InFile).endswith("global/qnamespace.h"))
              G.IsQtNamespace = true;
          G.GenerateCode();
        };

        Out << "QT_END_MOC_NAMESPACE\n";
    }
};

class MocAction : public clang::ASTFrontendAction {
protected:

    virtual clang::ASTConsumer *CreateASTConsumer(clang::CompilerInstance &CI,
                                           llvm::StringRef InFile) override {

        CI.getFrontendOpts().SkipFunctionBodies = true;
        CI.getPreprocessor().enableIncrementalProcessing(true);
        CI.getPreprocessor().SetSuppressIncludeNotFoundError(true);
        CI.getLangOpts().DelayedTemplateParsing = true;

        //enable all the extension
        CI.getLangOpts().MicrosoftExt = true;
        CI.getLangOpts().DollarIdents = true;
#if CLANG_VERSION_MAJOR != 3 || CLANG_VERSION_MINOR > 2
        CI.getLangOpts().CPlusPlus11 = true;
#else
        CI.getLangOpts().CPlusPlus0x = true;
#endif
        CI.getLangOpts().CPlusPlus1y = true;
        CI.getLangOpts().GNUMode = true;

        CI.getDiagnostics().setClient(new MocDiagConsumer{CI.getDiagnostics().takeClient()});

        return new MocNGASTConsumer(CI, InFile);
    }

public:
    // CHECK
    virtual bool hasCodeCompletionSupport() const { return true; }
};

static void showVersion(bool /*Long*/) {
    std::cerr << "moc-ng version " MOCNG_VERSION_STR " by Woboq [http://woboq.com]" << std::endl;
}

static void showHelp() {
    std::cerr << "Usage moc: [options] <header-file>\n"
              "  -o<file>           write output to file rather than stdout\n"
              "  -I<dir>            add dir to the include path for header files\n"
              "  -E                 preprocess only; do not generate meta object code\n"
              "  -D<macro>[=<def>]  define macro, with optional definition\n"
              "  -U<macro>          undefine macro\n"
              "  -i                 do not generate an #include statement\n"
//               "  -p<path>           path prefix for included file\n"
//               "  -f[<file>]         force #include, optional file name\n"
//               "  -nn                do not display notes\n"
//               "  -nw                do not display warnings\n"
//               "  @<file>            read additional options from file\n"
              "  -v                 display version of moc-ng\n"

/* undocumented options
              "  -include <file>    Adds an implicit #include into the predefines buffer which is read before the source file is preprocessed\n"
              "  -W<warnings>       Enable the specified warning\n"
              "  -f<option>         clang option\n"
              "  -X<ext> <arg>      extensions arguments\n"
*/



              << std::endl;


    showVersion(false);
}


int main(int argc, const char **argv)
{
  bool PreprocessorOnly = false;
  std::vector<std::string> Argv;
  Argv.push_back(argv[0]);
  Argv.push_back("-x");  // Type need to go first
  Argv.push_back("c++");
  Argv.push_back("-fPIE");
  Argv.push_back("-Wno-microsoft"); // get rid of a warning in qtextdocument.h

  Options.Output = "-";
  bool NextArgNotInput = false;
  bool HasInput = false;

  for (int I = 1 ; I < argc; ++I) {
    if (argv[I][0] == '-') {
        NextArgNotInput = false;
        switch (argv[I][1]) {
            case 'h':
            case '?':
                showHelp();
                return EXIT_SUCCESS;
            case 'v':
                showVersion(true);
                return EXIT_SUCCESS;
            case 'o':
                if (argv[I][2]) Options.Output = &argv[I][2];
                else if ((++I) < argc) Options.Output = argv[I];
                continue;
            case 'i':
                if (argv[I] == llvm::StringRef("-i")) {
                    Options.NoInclude = true;
                    continue;
                } else if (argv[I] == llvm::StringRef("-include")) {
                    NextArgNotInput = true;
                    break;
                }
                goto invalidArg;
            case 'E':
                PreprocessorOnly = true;
                break;
            case 'I':
            case 'U':
            case 'D':
                NextArgNotInput = (argv[I][2] == '\0');
                break;
            case 'X':
                NextArgNotInput = true;
                break;
            case 'f': //this is understood as compiler option rather than moc -f
            case 'W': // same
                break;
            default:
invalidArg:
                std::cerr << "moc-ng: Invalid argument '" << argv[I] << "'" << std::endl;
                showHelp();
                return EXIT_FAILURE;
        }
    } else if (!NextArgNotInput) {
        if (HasInput) {
            std::cerr << "error: Too many input files specified" << std::endl;
            return EXIT_FAILURE;
        }
        HasInput = true;
    }
    Argv.push_back(argv[I]);
  }

  //FIXME
  Argv.push_back("-I/usr/include/qt5");
  Argv.push_back("-I/usr/include/qt5/QtCore");
  Argv.push_back("-I/builtins");

  clang::FileManager FM({"."});

  if (PreprocessorOnly) {
      Argv.push_back("-P");
      clang::tooling::ToolInvocation Inv(Argv, new clang::PrintPreprocessedAction, &FM);
      return !Inv.run();
  }


  Argv.push_back("-fsyntax-only");

  clang::tooling::ToolInvocation Inv(Argv, new MocAction, &FM);

  const EmbeddedFile *f = EmbeddedFiles;
  while (f->filename) {
      Inv.mapVirtualFile(f->filename, {f->content , f->size } );
      f++;
  }

  return !Inv.run();
}




