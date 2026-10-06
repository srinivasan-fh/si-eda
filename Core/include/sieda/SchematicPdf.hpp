// SiEDA Core — schematic sheet templates (A4 … A0, ANSI A … E) and the PDF of every sheet with hierarchy bookmarks,
// frames with zone markers and title blocks.
#pragma once

#include <string>
#include <vector>

namespace sieda {

class Project;

/// A drawing sheet size, landscape (millimetres).
struct SheetTemplate {
    std::string name;  // "A4", "A3", "A2", "A1", "A0", "ANSI A" … "ANSI E"
    double widthMm = 0, heightMm = 0;
};
const std::vector<SheetTemplate>& sheetTemplates();
const SheetTemplate* findSheetTemplate(const std::string& name);

/// Millimetres per schematic unit (10 units = the 0.1 inch grid).
constexpr double kSchematicUnitMm = 0.254;

/// The template a sheet is printed on: its own (Sheet::size), else the smallest of A4 … A0 that holds its drawing at
/// full scale (A0, scaled down, when none does).
const SheetTemplate& sheetTemplateFor(const Project& project, int sheet);

/// PDF (1.4, uncompressed ASCII) of every schematic sheet in sheet order, one page per sheet on its template: a frame
/// with zone markers, the drawing (wires, buses, parts with designators and values, labels, ports, sheet symbols,
/// junctions) scaled to fit when it is larger than the sheet, and the title block (title, company, revision, date,
/// drawn by, sheet name, "Sheet n of N"). The document outline (bookmarks) follows the sheet hierarchy.
std::string exportSchematicPdf(const Project& project);

/// PDF options. `fontData`: a TrueType font file (.ttf / .ttc, glyf outlines) embedded as a subset for text the
/// standard fonts cannot show (CJK, Indic, Cyrillic …); without one such characters print as '?'. Latin text uses
/// Helvetica (WinAnsi), Greek letters and common math signs the Symbol font.
struct SchematicPdfOptions {
    std::string fontData;
};
std::string exportSchematicPdf(const Project& project, const SchematicPdfOptions& options);

}  // namespace sieda
