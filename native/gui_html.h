#pragma once
// 内嵌的前端界面（由 main.cpp 通过 WebView2 请求拦截喂给窗口，不读磁盘、不经过浏览器）。
//
// 界面自己做全部编排：把动画 HTML 注入隐藏 iframe，接管其中的 requestAnimationFrame
// 与 performance.now，按帧推进并用 canvas.toDataURL() 取回浏览器编码好的 PNG，
// 经 fetch('/api/...') 交给 C++ 写盘。于是只需一个 WebView2 实例，
// C++ 侧也不需要 PNG 编码器或图像库——这是 exe 能压到 300KB 以内的关键。
//
// 这个文件由 build_ui.js 从 gui.html 生成，请勿直接编辑；改界面请改 gui.html。
// HTML 超过 MSVC 单个字符串字面量上限（16380 字节），故拆成多块，
// C++ 会把相邻的字符串字面量自动拼接。

static const char* GUI_HTML =
    R"GUI0(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>序列帧导出</title>
<style>
  /* 桌面工具界面：白色主题 + 高密度。
     参数区用 grid + 内联流，不用嵌套 flex —— 嵌套 flex 在 WebView2 里
     会出现行高失控（行内容只有 30px 却占掉 70px 高度）。 */
  *{box-sizing:border-box;margin:0;padding:0}
  :root{
    --bg:#F8FAFC; --card:#FFF; --line:#E4E7EB; --line2:#F1F3F5;
    --tx:#0F172A; --mut:#475569; --dim:#94A3B8;
    --pri:#1E3A5F; --prih:#16304F; --acc:#059669; --err:#DC2626;
    --warn:#B45309; --warnbg:#FFFBEB; --warnline:#FDE68A;
  }
  html,body{height:100%}
  body{
    font:12px/1.45 "Segoe UI","Microsoft YaHei",-apple-system,sans-serif;
    background:var(--bg);color:var(--tx);
    -webkit-font-smoothing:antialiased;
    padding:9px;overflow-y:auto;
  }
  .card{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:9px}

  /* 选择区 */
  .drop{
    border:1.5px dashed #CBD5E1;border-radius:5px;padding:7px;text-align:center;
    cursor:pointer;transition:border-color .12s,background .12s;background:#FCFDFE;
  }
  .drop:hover,.drop.ov{border-color:var(--pri);background:#F0F5FA}
  .drop:focus-visible{outline:2px solid var(--pri);outline-offset:1px}
  .drop .t{font-size:12px;font-weight:600;line-height:17px}
  .drop .s{font-size:11px;color:var(--dim);margin-top:0;line-height:15px}

  .pr{display:flex;gap:5px;margin-top:5px}
  .pr input{
    flex:1;min-width:0;background:#fff;border:1px solid var(--line);border-radius:4px;
    color:var(--tx);padding:2px 7px;font:inherit;font-size:11.5px;height:24px;
  }
  .pr input::placeholder{color:var(--dim)}
  .pr input:focus{outline:none;border-color:var(--pri);box-shadow:0 0 0 2px rgba(30,58,95,.08)}
  .mini{
    background:#fff;border:1px solid var(--line);border-radius:4px;color:var(--tx);
    padding:0 10px;font:inherit;font-size:11.5px;cursor:pointer;white-space:nowrap;height:24px;
  }
  .mini:hover{background:var(--bg);border-color:#CBD5E1}
  .mini:disabled{opacity:.5;cursor:not-allowed}

  /* 参数区 */
  .meta{display:none;margin-top:6px;padding-top:6px;border-top:1px solid var(--line2)}
  .meta.on{display:block}
  .fn{font-size:11.5px;color:var(--mut);margin-bottom:2px;word-break:break-all;line-height:16px}
  .fn b{color:var(--acc);font-weight:600}

  /* 参数区：三列 grid（标签 | 值 | 提示）。
     值列必须给足宽度（用 min-content 会塌成 5px，input 被挤没）；
     提示列单独占一列并允许截断，否则长提示会在值格里换行把整行撑高。 */
  .pg{
    display:grid;grid-template-columns:44px 132px 1fr;gap:0 6px;align-items:center;
    padding-top:4px;border-top:1px solid var(--line2);
  }
  .pg .k{color:var(--mut);font-size:11.5px;line-height:20px}
  .pg .v{font-size:11.5px;font-weight:600;line-height:20px;white-space:nowrap;min-width:0}
  .pg .h{font-weight:400;color:var(--dim);font-size:10.5px;
         overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
  .pg input{
    width:46px;background:#fff;border:1px solid var(--line);border-radius:3px;
    color:var(--tx);padding:0 4px;font:inherit;font-size:11.5px;font-weight:600;
    text-align:right;height:18px;line-height:18px;vertical-align:middle;
  }
  .pg input::-webkit-outer-spin-button,.pg input::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
  .pg input[type=number]{-moz-appearance:textfield;appearance:textfield}
  .pg input:focus{outline:none;border-color:var(--pri);box-shadow:0 0 0 2px rgba(30,58,95,.08)}
  .sep{color:var(--dim);font-weight:400;margin:0 1px}

  .warn{
    display:none;margin-top:6px;background:var(--warnbg);border:1px solid var(--warnline);
    color:var(--warn);border-radius:4px;padding:4px 7px;font-size:11px;white-space:pre-wrap;
  }
  .out{margin-top:6px;font-size:11px;color:var(--dim);word-break:break-all}
  .out code{color:var(--mut);background:var(--bg);padding:0 3px;border-radius:2px;
            font-family:Consolas,monospace}

  /* 按钮 */
  .acts{display:flex;gap:6px;margin-top:8px}
  .btn{
    flex:1;background:var(--pri);color:#fff;border:1px solid var(--pri);border-radius:4px;
    padding:5px 10px;font:inherit;font-size:12px;font-weight:600;cursor:pointer;
    transition:background .12s;white-space:nowrap;
  }
  .btn:hover:not(:disabled){background:var(--prih)}
  .btn:focus-visible{outline:2px solid var(--pri);outline-offset:1px}
  .btn:disabled{opacity:.38;cursor:not-allowed}
  .btn.alt{background:#fff;color:var(--pri);border-color:var(--line)}
  .btn.alt:hover:not(:disabled){background:#F0F5FA;border-color:#CBD5E1}

  /* 进度与日志 */
  .bar{height:3px;background:var(--line2);border-radius:99px;overflow:hidden;margin-top:7px;display:none}
  .bar.on{display:block}
  .bar i{display:block;height:100%;width:0;background:var(--pri);transition:width .15s linear}
  .st{margin-top:3px;font-size:11px;color:var(--mut);display:none;justify-content:space-between;gap:6px}
  .st.on{display:flex}.st b{color:var(--tx);font-weight:600}
  .log{
    margin-top:6px;background:var(--bg);border:1px solid var(--line);border-radius:4px;
    padding:5px 7px;max-height:84px;overflow-y:auto;display:none;
    font:10.5px/1.55 Consolas,"Cascadia Mono",monospace;color:var(--mut);
    white-space:pre-wrap;word-break:break-all;
  }
  .log.on{display:block}
  .log .e{color:var(--err)}.log .o{color:var(--acc)}.log .w{color:var(--warn)}

  /* 隐藏的渲染舞台 */
  #stage{position:absolute;left:-99999px;top:0;width:1px;height:1px;overflow:hidden}
</style>
</head>
<body>
<div class="card">
  <div class="drop" id="drop" tabindex="0" role="button" aria-label="选择 HTML 文件">
    <div class="t">选择 HTML 文件</div>
    <div class="s">点击选择，或把文件拖到这里</div>
  </div>
  <div class="pr">
    <input type="text" id="pi" placeholder="粘贴文件路径（留空则点右侧按钮选择文件）" aria-label="文件路径">
    <button class="mini" id="use">浏览</button>
  </div>

  <div class="meta" id="meta">
    <div class="fn" id="fn"></div>
    <div class="pg">
      <span class="k">尺寸</span><span class="v" id="vsize">-</span><span class="h" id="hsize"></span>
      <span class="k">帧率</span><span class="v" id="vfps">-</span><span class="h" id="hfps"></span>
      <span class="k">时长</span><span class="v" id="vdur">-</span><span class="h" id="hdur"></span>
      <span class="k">总帧数</span><span class="v" id="vfr">-</span><span class="h"></span>
    </div>
    <div class="warn" id="warn"></div>
    <div class="out">导出到 <code id="od">-</code></div>
  </div>

  <div class="acts">
    <button class="btn" id="bPng" disabled>导出 PNG 序列帧</button>
    <button class="btn alt" id="bMp4" disabled>导出 MP4</button>
  </div>
  <div class="bar" id="bar"><i id="bf"></i></div>
  <div class="st" id="st"><span id="ph"></span><span id="ct"></span></div>
  <div class="log" id="log"></div>
</div>

<div id="stage"><iframe id="frame" title="渲染舞台"></iframe></div>
<input type="file" id="fi" accept=".html,.htm" style="display:none">

<script>
(function(){
  var E=function(i){return document.getElementById(i)};
  var S={text:null,name:null,info:null,outDir:null,busy:false,ff:false,stop:false};

  function log(m,c){var b=E('log');b.classList.add('on');var d=document.createElement('div');
    if(c)d.className=c;d.textContent=m;b.appendChild(d);b.scrollTop=b.scrollHeight}
  function bar(p){E('bar').classList.add('on');E('bf').style.width=Math.max(0,Math.min(100,p))+'%'}
  function st(a,b){E('st').classList.add('on');if(a!==undefined)E('ph').textContent=a;
    if(b!==undefined)E('ct').innerHTML=b}
  function busy(v){S.busy=v;E('bPng').disabled=v||!S.text;E('bMp4').disabled=v||!S.text||!S.ff}
  function base(n){return n.replace(/\.html?$/i,'').replace(/[\\/:*?"<>|]/g,'_')}
  function api(path,body){return fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},
)GUI0"
    R"GUI1(    body:JSON.stringify(body||{})}).then(function(r){return r.json()})}

  // ---------- 初始化 ----------
  api('/api/state').then(function(s){
    S.ff=!!(s.ffmpeg&&s.ffmpeg.ok);
    if(!S.ff)log('未检测到 ffmpeg，MP4 不可用（PNG 序列帧不受影响）。','w');
    return api('/api/pending');
  }).then(function(p){
    if(p&&p.file&&p.file.text)show(p.file.info,p.file.name,p.file.text);
  }).catch(function(e){log('初始化失败：'+e.message,'e')});

  // ---------- 选择文件 ----------
  var drop=E('drop');
  drop.onclick=function(){E('fi').click()};
  drop.onkeydown=function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();E('fi').click()}};
  drop.ondragover=function(e){e.preventDefault();drop.classList.add('ov')};
  drop.ondragleave=function(){drop.classList.remove('ov')};
  drop.ondrop=function(e){e.preventDefault();drop.classList.remove('ov');
    var f=e.dataTransfer.files&&e.dataTransfer.files[0];if(!f)return;
    var r=new FileReader();r.onload=function(){load(f.name,String(r.result))};r.readAsText(f,'utf-8')};
  E('fi').onchange=function(){var f=E('fi').files[0];if(!f)return;
    var r=new FileReader();r.onload=function(){load(f.name,String(r.result))};r.readAsText(f,'utf-8')};

  // 路径框为空时直接打开文件选择对话框；填了内容才按路径载入。
  E('use').onclick=function(){
    var p=E('pi').value.trim().replace(/^"|"$/g,'');
    if(!p){E('fi').click();return}
    E('use').disabled=true;
    api('/api/readfile',{path:p}).then(function(j){
      E('use').disabled=false;
      if(j.error){log('载入失败：'+j.error,'e');return}
      show(j.info,j.name,j.text);
    }).catch(function(e){E('use').disabled=false;log('载入失败：'+e.message,'e')});
  };
  E('pi').onkeydown=function(e){if(e.key==='Enter')E('use').click()};

  function load(name,text){
    api('/api/inspect',{htmlText:text}).then(function(i){show(i,name,text)})
      .catch(function(e){log('识别失败：'+e.message,'e')});
  }

  function show(i,name,text){
    S.info=i;S.name=name;S.text=text;
    E('fn').innerHTML='<b>已载入</b> '+name;
    E('meta').classList.add('on');
    E('log').innerHTML='';E('log').classList.remove('on');
    E('bar').classList.remove('on');E('st').classList.remove('on');
    E('vsize').innerHTML='<input type="number" id="iw" value="'+i.width+'">'+
      '<span class="sep">x</span><input type="number" id="ih" value="'+i.height+'">';
    E('hsize').textContent=i.sizeSource||'';
    E('vfps').innerHTML='<input type="number" id="ifps" value="'+i.fps+'"> fps';
    E('hfps').textContent=i.fpsSource||'';
    E('vdur').innerHTML='<input type="number" id="idur" value="'+(i.duration||70)+'" step="0.1"> 秒';
    ['iw','ih','ifps','idur'].forEach(function(id){var el=E(id);if(!el)return;
      el.oninput=function(){var v=parseFloat(el.value);if(!isFinite(v)||v<=0)return;
        if(id==='iw')i.width=Math.round(v);if(id==='ih')i.height=Math.round(v);
        if(id==='ifps')i.fps=Math.round(v);if(id==='idur')i.duration=v;frames()}});
    frames();
    var w=(i.warnings||[]).slice();
    if(!i.duration)w.push('未识别出时长，已按 70 秒填入，可修改。');
    if(w.length){E('warn').style.display='block';E('warn').textContent=w.join('\n')}
    else E('warn').style.display='none';
    E('od').textContent=(i.toolDir||'')+'\\'+base(name)+'_<时间戳>';
    busy(false);
  }
  function frames(){
    var f=parseInt(E('ifps')&&E('ifps').value,10)||30;
    var d=parseFloat(E('idur')&&E('idur').value)||70;
    E('vfr').textContent=Math.round(f*d)+' 帧';
    // 提示列内容由 tick 与当前 fps/时长共同决定，整体重算避免重复累加
    var parts=[];
    if(S.info&&S.info.tick)parts.push('每 '+S.info.tick.ms+'ms 变化');
    parts.push(f+' x '+d+'s');
    E('hdur').textContent=parts.join(' · ');
  }

  // ---------- 隐藏舞台：接管 iframe 里的帧循环 ----------
  function buildStage(html,w,h,fps){
    return new Promise(function(resolve,reject){
      var f=E('frame');
      f.onload=function(){
        try{
          var doc=f.contentDocument,win=f.contentWindow;
          doc.open();doc.write(html);doc.close();
          var c=doc.querySelector('canvas');
          if(c&&w>0&&h>0){c.width=w;c.height=h}
          setTimeout(function(){resolve({doc:doc,win:win})},50);
        }catch(e){reject(e)}
      };
      f.srcdoc='<!DOCTYPE html><html><head><meta charset="utf-8"><script>'+
        'window.__q=[];window.__t=0;'+
        'window.requestAnimationFrame=function(cb){window.__q.push(cb);return window.__q.length};'+
        'window.cancelAnimationFrame=function(){};'+
        'try{Object.defineProperty(window.performance,"now",{value:function(){return window.__t},writable:true,configurable:true})}catch(e){window.performance.now=function(){return window.__t}};'+
        'window.__step=function(ms){window.__t=ms;var b=window.__q;window.__q=[];'+
        'for(var i=0;i<b.length;i++){try{b[i](ms)}catch(e){window.__err=String(e&&e.message||e)}}return b.length};'+
        '<\/script></head><body></body></html>';
    });
  }

  function grabPng(win){
    try{
      var c=win.document.querySelector('canvas');
      if(!c)return null;
      var u=c.toDataURL('image/png');
      var i=u.indexOf(',');
      return i<0?null:u.slice(i+1);
    }catch(e){return null}
  }

  // ---------- 导出 ----------
  function go(mode){
    if(S.busy||!S.text)return;
    var i=S.info||{},fps=parseInt(E('ifps').value,10)||30,dur=parseFloat(E('idur').value)||70;
    var w=parseInt(E('iw').value,10)||i.width,h=parseInt(E('ih').value,10)||i.height;
    var total=Math.round(fps*dur);
    var makePng=(mode==='png'),makeMp4=(mode==='mp4');

    busy(true);E('log').innerHTML='';E('log').classList.remove('on');
    bar(0);st('准备中','');
    log('开始导出 '+(makePng?'PNG 序列帧':'MP4')+' · '+fps+'fps · '+dur+'s · '+total+' 帧');

    api('/api/export/begin',{sourceName:S.name,makePng:makePng,makeMp4:makeMp4,width:w,height:h,fps:fps})
    .then(function(b){
      if(b.error)throw new Error(b.error);
      S.outDir=b.outDir;
      log('导出目录 '+b.outDir);
      return buildStage(S.text,w,h,fps);
    })
    .then(function(stage){
      var t0=Date.now(),n=0;
      return new Promise(function(resolve,reject){
        function next(){
          if(S.stop){reject(new Error('已取消'));return}
          if(n>=total){resolve(n);return}
          n++;
          try{stage.win.__step(n*(1000/fps))}catch(e){}
          var b64=grabPng(stage.win);
          if(!b64){reject(new Error('无法从画布取图，页面可能没有 canvas。'));return}
          api('/api/export/frame',{index:n,data:b64}).then(function(){
            if(n===1||n%8===0||n===total){
              var el=(Date.now()-t0)/1000;
              var eta=n>0?(el/n)*(total-n):0;
              bar(n/total*100);
              st('渲染中','<b>'+n+'</b> / '+total+' · 剩 '+Math.round(eta)+'s');
            }
            next();
          }).catch(reject);
        }
        next();
      });
    })
    .then(function(){
      st('收尾中','');
      return api('/api/export/end',{});
    })
    .then(function(e){
      if(e.error)throw new Error(e.error);
      bar(100);st('完成','<b>'+e.frameCount+'</b> 帧');
      busy(false);
      log('完成：'+e.outDir,'o');
      if(e.mp4)log('视频：'+e.mp4,'o');
    })
    .catch(function(e){
      api('/api/export/abort',{});
      st('失败','');busy(false);log('失败：'+e.message,'e');
    });
  }
  E('bPng').onclick=function(){go('png')};
  E('bMp4').onclick=function(){go('mp4')};

  // ---------- 自检 ----------
  // 两种模式共用：__selftestSmoke 用内置动画验证程序本身，__selftestReal 用已载入的真实文件。
  function selfRun(html,w,h,fps,total,label){
    var lines=[];
    function step(n,ok,extra){lines.push((ok?'PASS':'FAIL')+' '+n+(extra?(' - '+extra):''));return ok}
    function report(){
)GUI1"
    R"GUI2(      fetch('/api/selftest',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({result:lines.join('\n'),outDir:S.outDir||''})});
    }
    if(!html){step('input',false,'没有可用的 HTML');report();return}
    step('input',true,label);
    step('canvas size',w>0&&h>0,w+'x'+h);
    api('/api/export/begin',{sourceName:'selftest.html',makePng:true,
      makeMp4:window.__selftestMp4===true,width:w,height:h,fps:fps})
    .then(function(b){
      if(!step('export begin',!b.error,b.error||b.outDir))throw new Error(b.error||'begin failed');
      S.outDir=b.outDir;
      return buildStage(html,w,h,fps);
    })
    .then(function(stage){
      return new Promise(function(res,rej){
        var n=0;
        function next(){
          if(n>=total){res(n);return}
          n++;
          try{stage.win.__step(n*(1000/fps))}catch(e){}
          var b64=grabPng(stage.win);
          if(!b64){rej(new Error('第 '+n+' 帧取图失败'));return}
          api('/api/export/frame',{index:n,data:b64}).then(function(r){
            if(r.error){rej(new Error(r.error));return}
            next();
          }).catch(rej);
        }
        next();
      });
    })
    .then(function(n){
      step('render frames',n>0,n+' 帧');
      return api('/api/export/end',{});
    })
    .then(function(e){
      step('export end',!e.error,e.error||('共 '+e.frameCount+' 帧'));
      report();
    })
    .catch(function(err){
      step('export',false,err.message);
      report();
    });
  }

  window.__selftestSmoke=function(){
    selfRun(window.__smokeHtml||'',8,8,30,3,'内置测试动画 8x8');
  };
  window.__selftestReal=function(){
    var i=S.info||{};
    if(!S.text){
      fetch('/api/selftest',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({result:'FAIL file loaded - 没有载入文件'})});
      return;
    }
    var w=i.width||0,h=i.height||0,fps=i.fps||30;
    var extra=S.name+' | 时长 '+(i.duration||0)+'s'+(i.tick?(' | tick '+i.tick.ms+'ms'):'')+
              ' | 尺寸来源 '+(i.sizeSource||'?');
    selfRun(S.text,w,h,fps,window.__selftestMp4===true?15:3,extra);
  };
})();
</script>
</body>
</html>

)GUI2";
