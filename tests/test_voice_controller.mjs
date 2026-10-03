import {test} from 'node:test';
import assert from 'node:assert/strict';
import {VoiceController} from '../voice-controller.mjs';
function setup(){
  const engines=[],sent=[],states=[],errors=[],timers=new Map();let next=0,text='';
  const c=new VoiceController({createRecognition:()=>{const e={start(){},stop(){},abort(){}};engines.push(e);return e},onText:s=>text=s,onState:s=>states.push(s),onError:s=>errors.push(s),send:async s=>sent.push(s),setTimer:(fn,ms)=>{timers.set(++next,{fn,ms});return next},clearTimer:id=>timers.delete(id)});
  return {c,engines,sent,states,errors,timers,get text(){return text},words(s){engines.at(-1).onresult({results:[[{transcript:s}]]})}};
}
test('stop sends final transcript once, including last words before end',async()=>{
 const t=setup();t.c.toggle();t.words('make');t.c.toggle();t.words('make something cool');const r=t.engines[0];r.onend();r.onend();await Promise.resolve();assert.deepEqual(t.sent,['make something cool']);assert.equal(t.c.state,'ready');
});
test('cancel never sends and ignores delayed speech callbacks',()=>{
 const t=setup();t.c.start();t.words('private draft');const r=t.engines[0];t.c.cancel();r.onresult({results:[[{transcript:'late'}]]});r.onend();assert.deepEqual(t.sent,[]);assert.equal(t.text,'');
});
test('silence restart keeps words across recognition runs',async()=>{
 const t=setup();t.c.start();t.words('hello');t.engines[0].onerror({error:'no-speech'});t.engines[0].onend();const restart=[...t.timers.values()].find(x=>x.ms===200);restart.fn();t.words('Muse');t.c.stop();t.engines[1].onend();await Promise.resolve();assert.deepEqual(t.sent,['hello Muse']);
});
test('no speech does not submit an empty turn',async()=>{
 const t=setup();t.c.start();t.c.stop();t.engines[0].onend();await Promise.resolve();assert.deepEqual(t.sent,[]);assert.match(t.errors[0],/No speech/);
});
test('timeout cancels without sending partial speech',()=>{
 const t=setup();t.c.start();t.words('unfinished');[...t.timers.values()].find(x=>x.ms===120000).fn();assert.deepEqual(t.sent,[]);assert.equal(t.c.state,'ready');
});
test('recognition permission error cancels without sending',()=>{
 const t=setup();t.c.start();t.words('draft');t.engines[0].onerror({error:'not-allowed'});assert.deepEqual(t.sent,[]);assert.equal(t.c.state,'ready');assert.match(t.errors[0],/not-allowed/);
});
test('stop fallback cannot duplicate a late final event',async()=>{
 const t=setup();t.c.start();t.words('hello Muse');t.c.stop();const r=t.engines[0];[...t.timers.values()].find(x=>x.ms===2500).fn();r.onend();await Promise.resolve();assert.deepEqual(t.sent,['hello Muse']);
});
test('default timers retain the browser global receiver through start, stop and cancel',async()=>{
 const oldSet=globalThis.setTimeout,oldClear=globalThis.clearTimeout;
 const timers=new Map(),sent=[];let next=0,r;
 globalThis.setTimeout=function(fn,ms){assert.equal(this,globalThis);timers.set(++next,{fn,ms});return next};
 globalThis.clearTimeout=function(id){assert.equal(this,globalThis);timers.delete(id)};
 try {
  const c=new VoiceController({createRecognition:()=>r={start(){},stop(){},abort(){}},onText(){},onState(){},onError(){},send:async text=>sent.push(text)});
  c.start();r.onresult({results:[[{transcript:'What should I do next?'}]]});c.stop();r.onend();await Promise.resolve();
  assert.deepEqual(sent,['What should I do next?']);assert.equal(c.state,'ready');
  c.start();c.cancel();assert.equal(c.state,'ready');assert.equal(timers.size,0);
 }finally{globalThis.setTimeout=oldSet;globalThis.clearTimeout=oldClear}
});
