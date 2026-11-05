Compareel languages

Each language is one JSON file in this folder. To add a language:

1. Copy template.json to a new name, for example de.json.
2. Fill in "language" (the name shown in Settings, e.g. "Deutsch") and "code" (e.g. "de").
3. Translate the value on the right of each line. Keep %1, %2 ... and \n exactly as they are.
4. Restart Compareel and choose the language in Settings.

Right-to-left languages such as Arabic and Persian mirror the whole window automatically.
A line you leave untranslated is shown in English. A file here with the same code as a
built-in language (tr) replaces it, so corrections can be tested without rebuilding.
Save the file as UTF-8.
