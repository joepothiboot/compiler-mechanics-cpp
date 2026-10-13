// Single-file app. Content lives in STAGES / OPTS / QUIZ below; edit those, not the renderers.
const STAGES = [
  {id:"01_ir_and_ssa", name:"IR + SSA", io:"AST → SSA IR", files:["04_ir_data_structures","05_ssa_construction"],
   why:"Everything downstream assumes SSA: dominators, dominance frontiers, φ placement, renaming.", needs:[]},
  {id:"02_analysis", name:"Analysis", io:"SSA → dataflow facts", files:["06_dataflow_analysis"],
   why:"Liveness and constant lattices. Liveness feeds register allocation; the lattice framework is what SCCP/range analysis instantiate.", needs:["01_ir_and_ssa"]},
  {id:"03_lowering", name:"Lowering", io:"SSA → machine ops (vregs, no φ)", files:["08_phi_elimination","09_instruction_selection"],
   why:"φ must be gone before regalloc, which has no notion of φ.", needs:["01_ir_and_ssa"]},
  {id:"04_scheduling", name:"Scheduling", io:"ops → reordered, laid-out blocks", files:["11_scheduling_and_layout"],
   why:"Reorders ops and places blocks so hot paths fall through.", needs:["03_lowering"]},
  {id:"05_register_alloc", name:"Register alloc", io:"vregs → phys regs + spills", files:["10_live_intervals_linear_scan","07_register_allocation"],
   why:"Needs liveness (02) and φ-free code (03). Spills create stack slots.", needs:["02_analysis","03_lowering"]},
  {id:"06_frame_and_peephole", name:"Frame + peephole", io:"allocated code → final code", files:["12_stack_frame_abi","13_peephole"],
   why:"Frame size is only known after spills; peepholes run last because earlier stages create the patterns they clean.", needs:["05_register_alloc"]},
];
// prio 1 = foundation (do/learn first), 3 = situational. files = study files that implement the prerequisite machinery.
const OPTS = [
  {ch:1, doc:"01_ir_ssa_cleanups", name:"Const folding/propagation, DCE, CSE, GVN", prio:1, stage:"01_ir_and_ssa", files:["05_ssa_construction"], note:"Almost every later pass assumes these already ran."},
  {ch:2, doc:"02_control_flow", name:"Jump threading, tail merging, SCCP", prio:1, stage:"02_analysis", files:["06_dataflow_analysis"], note:"SCCP is a lattice instance of the dataflow framework."},
  {ch:3, doc:"03_loop_transformations", name:"LICM, unrolling, unswitching, fusion, interchange, tiling", prio:2, stage:"01_ir_and_ssa", files:["05_ssa_construction","06_dataflow_analysis"], note:"Needs dominators (loops) + SSA."},
  {ch:4, doc:"04_strength_reduction_vectorization", name:"Strength reduction, IV elimination, SIMD", prio:2, stage:"03_lowering", files:["09_instruction_selection"], note:"Depends on loop canonical form from ch.3."},
  {ch:5, doc:"05_alias_analysis_memory", name:"TBAA, restrict, load forwarding, escape analysis", prio:2, stage:"02_analysis", files:["06_dataflow_analysis"], note:"Gates how aggressive ch.3–4 can be."},
  {ch:6, doc:"06_bounds_check_elimination", name:"Bounds-check elimination", prio:3, stage:"02_analysis", files:["06_dataflow_analysis"], note:"Range analysis on top of branch conditions."},
  {ch:7, doc:"07_interprocedural", name:"Inlining, specialization, LTO, PGO", prio:1, stage:"01_ir_and_ssa", files:["04_ir_data_structures"], note:"Inlining first: it exposes everything else."},
  {ch:8, doc:"08_hardware_level", name:"FMA, alignment, cache layout, false sharing", prio:3, stage:"06_frame_and_peephole", files:["12_stack_frame_abi","13_peephole"], note:"Target-specific, last."},
];
const QUIZ = [
  {q:"Why must φ elimination run before register allocation?", a:["Regalloc has no notion of φ","φ is slower than moves","Spills need φ","Scheduling removes φ"], c:0},
  {q:"What does liveness analysis feed?", a:["Frame layout","Register allocation","Instruction selection","Parsing"], c:1},
  {q:"Why is the frame finalized after allocation?", a:["Spills create stack slots","ABI changes at runtime","Peepholes need it","Dominators change"], c:0},
  {q:"Where is a φ for variable x placed?", a:["At every block","At the iterated dominance frontier of x's definitions","At function entry","Only in loop headers"], c:1},
  {q:"Why do peepholes run last?", a:["They are cheapest","Earlier stages create the patterns they clean up","They need SSA","They set the ABI"], c:1},
  {q:"Which chapter's passes do nearly all later passes assume ran first?", a:["Ch.1–2 cleanups & control flow","Ch.8 hardware","Ch.6 bounds checks","Ch.3 loops"], c:0},
];
const $ = (h) => { const t = document.createElement("template"); t.innerHTML = h.trim(); return t.content.firstChild; };
const view = document.getElementById("view");
const stageOf = (id) => STAGES.find(s => s.id === id);
const stageOfFile = (f) => STAGES.find(s => s.files.includes(f));
const tabs = {
  pipeline(el) {
    let sel = STAGES[0];
    const draw = () => {
      el.innerHTML = `<p class="mut">Click a stage. Arrows = hard order constraints.</p>
      <div class="flow">${STAGES.map((s,i) => `<button class="stage ${s===sel?"on":""}" data-i="${i}"><small>${i+1}</small><b>${s.name}</b><small>${s.io}</small></button>`).join("")}</div>
      <div class="card"><h3>${sel.name}</h3><p>${sel.why}</p>
      <p><b>Needs:</b> ${sel.needs.map(n=>`<span class="tag">${stageOf(n).name}</span>`).join("")||"<span class=mut>nothing</span>"}</p>
      <p><b>Unlocks:</b> ${STAGES.filter(s=>s.needs.includes(sel.id)).map(s=>`<span class="tag">${s.name}</span>`).join("")||"<span class=mut>end of pipeline</span>"}</p>
      <p><b>Code:</b> ${sel.files.map(f=>`<code>compiler/${sel.id}/${f}.cpp</code>`).join("<br>")}</p>
      <p><b>Optimizations that lean on it:</b></p><ul>${OPTS.filter(o=>o.stage===sel.id||o.files.some(f=>sel.files.includes(f))).map(o=>`<li class="item"><span>Ch.${o.ch} ${o.name}</span><span class="tag p${o.prio}">prio ${o.prio}</span></li>`).join("")||"<li class=mut>none mapped</li>"}</ul></div>`;
      el.querySelectorAll(".stage").forEach(b => b.onclick = () => { sel = STAGES[b.dataset.i]; draw(); });
    };
    draw();
  },
  opts(el) {
    el.innerHTML = `<p class="mut">prio 1 = learn/apply first (foundation), 3 = situational. Files = where the prerequisite machinery lives in this repo.</p>
    <table><tr><th>Ch</th><th>Techniques</th><th>Prio</th><th>Code</th><th>Why</th></tr>${[...OPTS].sort((a,b)=>a.prio-b.prio||a.ch-b.ch).map(o=>`<tr><td><a href="../docs/optimization_guide/${o.doc}.md">${o.ch}</a></td><td>${o.name}</td><td><span class="tag p${o.prio}">${o.prio}</span></td><td>${o.files.map(f=>`<code>${f}</code>`).join("<br>")}</td><td class="mut">${o.note}</td></tr>`).join("")}</table>`;
  },
  order(el) {
    let list = STAGES.map(s=>s.id).sort(()=>Math.random()-.5), msg = "";
    const draw = () => {
      el.innerHTML = `<p>Arrange the stages so every dependency comes before what needs it.</p><ol>${list.map((id,i)=>`<li class="item"><span>${stageOf(id).name} <small class="mut">${stageOf(id).io}</small></span><button data-i="${i}" data-d="-1">↑</button><button data-i="${i}" data-d="1">↓</button></li>`).join("")}</ol>
      <button class="pri" id="chk">Check</button> <span id="msg">${msg}</span>`;
      el.querySelectorAll("[data-d]").forEach(b => b.onclick = () => { const i=+b.dataset.i,j=i+ +b.dataset.d; if(j<0||j>=list.length)return; [list[i],list[j]]=[list[j],list[i]]; msg=""; draw(); });
      el.querySelector("#chk").onclick = () => {
        const bad = list.flatMap((id,i)=>stageOf(id).needs.filter(n=>list.indexOf(n)>i).map(n=>`${stageOf(id).name} needs ${stageOf(n).name} first`));
        msg = bad.length ? `<span class=bad>${bad.join("; ")}</span>` : `<span class=ok>Valid order ✓ (several are valid — the repo's is 01→06)</span>`; draw();
      };
    };
    draw();
  },
  quiz(el) {
    let i = 0, score = 0;
    const draw = () => {
      if (i >= QUIZ.length) { el.innerHTML = `<div class="card"><h3>${score}/${QUIZ.length}</h3><button class="pri" id="r">Again</button></div>`; el.querySelector("#r").onclick = () => { i=0; score=0; draw(); }; return; }
      const q = QUIZ[i];
      el.innerHTML = `<div class="card"><p class="mut">${i+1}/${QUIZ.length}</p><h3>${q.q}</h3>${q.a.map((a,k)=>`<button class="opt" data-k="${k}">${a}</button>`).join("")}</div>`;
      el.querySelectorAll(".opt").forEach(b => b.onclick = () => {
        const k = +b.dataset.k; if (k===q.c) score++;
        el.querySelectorAll(".opt").forEach(o => { o.disabled = true; o.classList.add(+o.dataset.k===q.c?"right":+o.dataset.k===k?"wrong":"x"); });
        el.firstChild.append($(`<button class="pri">Next</button>`)); el.querySelector(".pri").onclick = () => { i++; draw(); };
      });
    };
    draw();
  },
  run(el) {
    const all = STAGES.flatMap(s => s.files.map(f => ({f, s})));
    el.innerHTML = `<p class="mut">Runs the real C++ test for a study file, compiled to WASM. Build once: <code>web/build_wasm.sh</code> (needs emscripten). Serve with <code>python3 -m http.server</code> from the repo root and open <code>/web/</code>.</p>
    <select id="f">${all.sort((a,b)=>a.f.localeCompare(b.f)).map(x=>`<option value="${x.f}">${x.f}  (${x.s.name})</option>`).join("")}</select> <button class="pri" id="go">Run</button>
    <pre id="out">(no output yet)</pre>`;
    const out = el.querySelector("#out");
    el.querySelector("#go").onclick = async () => {
      const f = el.querySelector("#f").value; let log = "";
      const put = (t) => { log += t + "\n"; out.textContent = log; };
      out.textContent = "loading…";
      try {
        await new Promise((ok, no) => { const s = document.createElement("script"); s.src = `wasm/${f}.js?${Date.now()}`; s.onload = ok; s.onerror = () => no(new Error(`wasm/${f}.js not found — run web/build_wasm.sh`)); document.head.append(s); });
        await createMod({print: put, printErr: put});
        out.insertAdjacentHTML("beforeend", /\[FAIL\]|FAIL /.test(log) ? "\nRESULT: FAIL" : "\nRESULT: PASS");
      } catch (e) { out.textContent = String(e.message || e); }
    };
  },
};
const nav = document.querySelectorAll("nav button");
const show = (t) => { nav.forEach(b => b.classList.toggle("on", b.dataset.t===t)); view.innerHTML=""; tabs[t](view); location.hash = t; };
nav.forEach(b => b.onclick = () => show(b.dataset.t));
show(tabs[location.hash.slice(1)] ? location.hash.slice(1) : "pipeline");
