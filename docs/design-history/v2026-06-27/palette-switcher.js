(function(){
  var PRESETS=[
    {id:'green',  name:'Зелёный',   accent:'#9bdb4d', rgb:'155,219,77'},
    {id:'teal',   name:'Бирюзовый', accent:'#67d5b5', rgb:'103,213,181'},
    {id:'amber',  name:'Жёлтый',    accent:'#e3b04c', rgb:'227,176,76'},
    {id:'indigo', name:'Синий',     accent:'#8ea7ff', rgb:'142,167,255'}
  ];
  var KEY='aswf-accent';
  function apply(p){
    var r=document.documentElement;
    r.style.setProperty('--accent', p.accent);
    r.style.setProperty('--accent-dim', 'rgba('+p.rgb+',.12)');
  }
  var saved=null;
  try{ saved=localStorage.getItem(KEY); }catch(e){}
  var cur=PRESETS.find(function(p){return p.id===saved;})||PRESETS[0];
  apply(cur);

  function build(){
    var bar=document.createElement('div');
    bar.id='aswf-pal-bar';
    bar.style.cssText='position:sticky;top:0;left:0;right:0;z-index:9999;background:#fff;color:#222;border-bottom:1px solid #d6d8d4;padding:8px 14px;display:flex;align-items:center;gap:12px;font-family:ui-monospace,Consolas,monospace;font-size:12px;box-shadow:0 1px 4px rgba(0,0,0,.18)';
    var label=document.createElement('span');
    label.textContent='Палитра:';
    label.style.cssText='color:#444;font-weight:600;';
    bar.appendChild(label);
    PRESETS.forEach(function(p){
      var b=document.createElement('button');
      b.title=p.name;
      b.dataset.id=p.id;
      b.style.cssText='width:30px;height:22px;border-radius:5px;border:2px solid '+(p.id===cur.id?'#222':'#d6d8d4')+';background:'+p.accent+';cursor:pointer;padding:0;position:relative;outline:none;';
      if(p.id===cur.id){
        var chk=document.createElement('span');
        chk.textContent='✓';
        chk.style.cssText='position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#0a0c0a;font-weight:700;font-size:13px;';
        b.appendChild(chk);
      }
      b.onclick=function(){
        cur=p; apply(p);
        try{ localStorage.setItem(KEY, p.id); }catch(e){}
        bar.remove(); build();
      };
      bar.appendChild(b);
    });
    var hint=document.createElement('span');
    hint.textContent='сохраняется в браузере, применяется на всех 4 страницах mock';
    hint.style.cssText='color:#888;margin-left:auto;';
    bar.appendChild(hint);
    document.body.insertBefore(bar, document.body.firstChild);
  }
  if(document.readyState==='loading'){
    document.addEventListener('DOMContentLoaded', build);
  }else{
    build();
  }
})();