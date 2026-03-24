const vscode = require("vscode");

function createSvgBar(colorHex) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 14 14"><rect x="6" y="1" width="2" height="12" rx="1" fill="${colorHex}"/></svg>`;
  return `data:image/svg+xml;utf8,${encodeURIComponent(svg)}`;
}

function createDecorationType({ colorHex }) {
  return vscode.window.createTextEditorDecorationType({
    isWholeLine: true,
    overviewRulerColor: colorHex,
    overviewRulerLane: vscode.OverviewRulerLane.Left,
    gutterIconSize: "contain",
    gutterIconPath: vscode.Uri.parse(createSvgBar(colorHex))
  });
}

function parseSections(document) {
  const sections = [];
  const sectionHeader = /^\s*\/\s*(cpp|python|py|javascript|js|main)\s*$/i;

  let current = null;
  for (let i = 0; i < document.lineCount; i += 1) {
    const text = document.lineAt(i).text;
    const match = text.match(sectionHeader);

    if (!match) {
      continue;
    }

    if (current) {
      current.endLine = i - 1;
      sections.push(current);
    }

    const rawLang = match[1].toLowerCase();
    const normalized = rawLang === "py" ? "python" : rawLang === "js" ? "javascript" : rawLang;
    const mappedLang = normalized === "main" ? "python" : normalized;

    current = {
      lang: mappedLang,
      startLine: i,
      endLine: document.lineCount - 1
    };
  }

  if (current) {
    sections.push(current);
  }

  return sections;
}

function buildRanges(document, sectionLanguage) {
  const ranges = [];
  const sections = parseSections(document);

  for (const section of sections) {
    if (section.lang !== sectionLanguage) {
      continue;
    }

    const start = Math.max(section.startLine, 0);
    const end = Math.max(section.endLine, section.startLine);
    ranges.push(
      new vscode.Range(
        new vscode.Position(start, 0),
        new vscode.Position(end, document.lineAt(end).text.length)
      )
    );
  }

  return ranges;
}

function updateDecorations(editor, decorationMap) {
  if (!editor || editor.document.languageId !== "poly") {
    return;
  }

  editor.setDecorations(decorationMap.cpp, buildRanges(editor.document, "cpp"));
  editor.setDecorations(decorationMap.javascript, buildRanges(editor.document, "javascript"));
  editor.setDecorations(decorationMap.python, buildRanges(editor.document, "python"));
}

function activate(context) {
  const decorationMap = {
    cpp: createDecorationType({
      // C++ => dark blue
      colorHex: "#0D47A1"
    }),
    javascript: createDecorationType({
      // JS => yellow
      colorHex: "#FBC02D"
    }),
    python: createDecorationType({
      // Python/main => light blue
      colorHex: "#42A5F5"
    })
  };

  context.subscriptions.push(decorationMap.cpp, decorationMap.javascript, decorationMap.python);

  const refresh = (editor = vscode.window.activeTextEditor) => {
    updateDecorations(editor, decorationMap);
  };

  context.subscriptions.push(
    vscode.window.onDidChangeActiveTextEditor((editor) => refresh(editor)),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (vscode.window.activeTextEditor && event.document === vscode.window.activeTextEditor.document) {
        refresh(vscode.window.activeTextEditor);
      }
    }),
    vscode.workspace.onDidOpenTextDocument((document) => {
      if (document.languageId === "poly" && vscode.window.activeTextEditor && document === vscode.window.activeTextEditor.document) {
        refresh(vscode.window.activeTextEditor);
      }
    })
  );

  refresh();
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
