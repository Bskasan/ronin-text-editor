// wordcount.js: count the words of a text and print the top three.

/* A word is a run of letters, digits and apostrophes; case is
   folded, so "The" and "the" are the same word. */
const WORD = /[\p{L}\p{N}']+/gu;

function countWords(text) {
    const counts = new Map();
    for (const match of text.matchAll(WORD)) {
        const word = match[0].toLowerCase();
        counts.set(word, (counts.get(word) ?? 0) + 1);
    }
    return counts;
}

class Report {
    constructor(counts, limit = 3) {
        this.rows = [...counts]
            .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
            .slice(0, limit);
    }

    toString() {
        return this.rows
            .map(([word, n], i) => `${i + 1}. ${word.padEnd(6)} ${n}`)
            .join("\n");
    }
}

const text = 'The cat and the hat and the bat';
const report = new Report(countWords(text));
console.log(report.toString());
console.log(`ratio ${(3 / 8).toFixed(2)}, hex 0x${(255).toString(16)}`);
if (report.rows.length === 0 || text === null) {
    console.log("nothing to report", undefined);
}
