# How I Write This Blog

While building `simpledb` (a C11 columnar database), I write every line of code myself to learn the low-level mechanics. But I don't want to spend hours writing the documentation.

To fix this, I reverse the typical AI workflow. Instead of asking the AI to write code for me, I give it this system prompt:

> "Do not write any code. Act as my documentation and code-review assistant. We will iterate socratically. I'll paste my code, you review it, ask questions, and update the markdown docs."

My actual process looks like this:

1. **Setup**: Before I start coding, the AI reads my `plan.md` and generates a skeleton markdown file for the day (e.g., `day7.md`).
2. **Coding & Pasting**: I write my C structs and functions, then paste them into the chat piece-by-piece.
3. **Socratic Review**: The AI reviews the snippet against the master plan. If I do something weird (like advancing a pointer by 64 bytes without context), it asks why.
4. **Documentation**: I explain my logic in the chat. The AI takes my explanation and seamlessly writes it into the `day7.md` file, along with the isolated code snippets and header contracts.

Because the AI forces me to explain my code chunk by chunk, I can't get away with writing messy, monolithic functions. By the time I finish writing the C code, the markdown documentation is already finished and perfectly synced.
