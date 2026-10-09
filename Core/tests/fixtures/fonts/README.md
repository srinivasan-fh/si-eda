# PDF shaping test fonts

Subsets of Noto Sans Devanagari and Noto Sans Arabic (SIL Open Font License 1.1, © Google / the Noto project),
holding only the glyphs and layout tables the `pdf_text_shaping` test needs:

    pyftsubset NotoSansDevanagari-Regular.ttf --text="क्षहिन्दी " --no-hinting --layout-features='*' --output-file=NotoSansDevanagari-subset.ttf
    pyftsubset NotoSansArabic-Regular.ttf --text="سلام " --no-hinting --layout-features='*' --output-file=NotoSansArabic-subset.ttf
