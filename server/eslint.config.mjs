// ESLint flat config for the work server: `npm run lint`. TypeScript under src/
// is linted with type information from tsconfig.json; the .mjs scripts and
// tests and the static page are outside tsconfig, so they get the
// syntax-only rules.
import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "eslint/config";
import js from "@eslint/js";
import tseslint from "typescript-eslint";

const here = path.dirname(fileURLToPath(import.meta.url));

const readonly = (names) => Object.fromEntries(names.map((n) => [n, "readonly"]));
const shared = ["console", "URL", "URLSearchParams", "TextEncoder", "TextDecoder", "AbortController",
    "fetch", "Request", "Response", "Headers", "crypto", "atob", "btoa", "structuredClone",
    "setTimeout", "clearTimeout", "setInterval", "clearInterval", "queueMicrotask"];
const nodeGlobals = readonly([...shared, "process", "Buffer", "global", "setImmediate"]);
const browserGlobals = readonly([...shared, "window", "document", "location", "navigator",
    "localStorage", "history", "requestAnimationFrame", "Intl"]);

export default defineConfig(
    { ignores: [".wrangler/**", ".test-state/**", "node_modules/**"] },
    js.configs.recommended,
    tseslint.configs.recommendedTypeChecked,
    {
        languageOptions: { parserOptions: { projectService: true, tsconfigRootDir: here } },
        rules: {
            "no-useless-catch": "error",
            "no-useless-return": "error",
            "no-useless-rename": "error",
            "no-useless-concat": "error",
            "no-lone-blocks": "error",
            "no-else-return": "error",
            "no-console": ["error", { allow: ["warn", "error"] }],
            // "placeholder" is left out: unset wrangler.jsonc values are called that.
            "no-warning-comments": ["error", {
                terms: ["in a real implementation", "todo: implement"],
                location: "anywhere",
            }],
            // The same 200-line limit as readability-function-size in .clang-tidy.
            "max-lines-per-function": ["warn", { max: 200, skipBlankLines: true, skipComments: true }],
            "id-match": ["error", "^(?![eE]\\d{2,3}_)"],
            // result and obj are names of things here: the result document and
            // the ledger field, and a JSON object.
            "id-denylist": ["warn", "data", "temp"],
            // Destructuring with a rest element drops the named keys on purpose.
            "@typescript-eslint/no-unused-vars": ["error", { ignoreRestSiblings: true }],
            "@typescript-eslint/no-unnecessary-condition": "error",
            "@typescript-eslint/no-unnecessary-type-assertion": "error",
            "@typescript-eslint/no-inferrable-types": "error",
            "@typescript-eslint/no-extraneous-class": "error",
            "@typescript-eslint/no-useless-constructor": "error",
            "@typescript-eslint/no-explicit-any": "error",
        },
    },
    {
        files: ["**/*.mjs", "**/*.js"],
        extends: [tseslint.configs.disableTypeChecked],
        languageOptions: { globals: nodeGlobals },
        rules: { "no-console": "off" },
    },
    {
        files: ["public/**/*.js"],
        languageOptions: { sourceType: "script", globals: browserGlobals },
    },
);
