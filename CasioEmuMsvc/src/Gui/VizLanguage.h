#pragma once
#include <string>
#include "imgui/TextEditor.h"

namespace VizLang {
    inline const TextEditor::LanguageDefinition& Definition() {
        static TextEditor::LanguageDefinition lang = []() {
            TextEditor::LanguageDefinition def;
            def.mName = "Viz";
            def.mKeywords.insert("void");
            def.mKeywords.insert("int");
            def.mKeywords.insert("def");
            def.mKeywords.insert("if");
            def.mKeywords.insert("while");
            def.mKeywords.insert("for");
            def.mKeywords.insert("return");
            def.mKeywords.insert("clear");
            def.mKeywords.insert("print");
            def.mKeywords.insert("delay");
            def.mKeywords.insert("key_hit");
            def.mKeywords.insert("str");
            def.mKeywords.insert("hex");
            def.mKeywords.insert("num");
            def.mPreprocIdentifiers["cwapp"] = TextEditor::Identifier();
            def.mPreprocIdentifiers["slot"] = TextEditor::Identifier();
            def.mPreprocIdentifiers["rop"] = TextEditor::Identifier();
            def.mPreprocIdentifiers["for"] = TextEditor::Identifier();
            def.mCommentStart = "/*";
            def.mCommentEnd = "*/";
            def.mSingleLineComment = "//";
            def.mPreprocChar = '#';
            def.mAutoIndentation = true;
            return def;
        }();
        return lang;
    }
}
