export class VoiceController {
  constructor({createRecognition, onText, onState, onError, send, setTimer=(...args)=>globalThis.setTimeout(...args), clearTimer=(...args)=>globalThis.clearTimeout(...args)}) {
    Object.assign(this,{createRecognition,onText,onState,onError,send,setTimer,clearTimer});
    this.state='ready'; this.recognition=null; this.generation=0; this.prefix=''; this.text='';
  }
  change(state){this.state=state;this.onState(state)}
  toggle(){if(this.state==='ready')this.start();else if(this.state==='listening')this.stop()}
  start(){
    if(this.state!=='ready')return;
    this.generation++;this.prefix='';this.text='';this.onText('');
    this.change('listening');this.run(this.generation);
    if(this.state==='listening')this.limit=this.setTimer(()=>{if(this.state==='listening'){this.onError('Two-minute limit reached. Press A+B to start a new recording.');this.cancel()}},120000);
  }
  run(gen){
    if(gen!==this.generation || this.state!=='listening')return;
    try {
      const r=this.createRecognition();this.recognition=r;
      r.lang='en-US';r.continuous=true;r.interimResults=true;
      r.onresult=e=>{
        if(gen!==this.generation)return;
        const words=Array.from(e.results).map(x=>x[0].transcript).join(' ').trim();
        this.text=[this.prefix,words].filter(Boolean).join(' ');this.onText(this.text);
      };
      r.onerror=e=>{if(gen===this.generation && e.error!=='no-speech'){this.onError('Microphone: '+e.error);this.cancel()}};
      r.onend=()=>{
        if(gen!==this.generation)return;
        this.recognition=null;
        if(this.state==='stopping')this.finish(gen);
        else if(this.state==='listening'){
          this.prefix=this.text;
          this.restart=this.setTimer(()=>this.run(gen),200);
        }
      };
      r.start();
    }catch(e){this.onError(e.message);this.cancel()}
  }
  stop(){
    if(this.state!=='listening')return;
    this.change('stopping');this.clearTimer(this.restart);this.clearTimer(this.limit);
    const gen=this.generation;
    this.fallback=this.setTimer(()=>this.finish(gen),2500);
    if(this.recognition){try{this.recognition.stop()}catch{this.finish(gen)}}
    else this.finish(gen);
  }
  async finish(gen){
    if(gen!==this.generation || this.state!=='stopping')return;
    this.clearTimer(this.fallback);this.clearTimer(this.limit);this.clearTimer(this.restart);
    this.generation++;const text=this.text.trim(),r=this.recognition;this.recognition=null;
    try{r?.abort()}catch{}
    try{if(text)await this.send(text);else this.onError('No speech detected. Press A+B and try again.')}
    catch(e){this.onError(e.message)}
    finally{this.change('ready')}
  }
  cancel(){
    this.generation++;this.clearTimer(this.fallback);this.clearTimer(this.limit);this.clearTimer(this.restart);
    const r=this.recognition;this.recognition=null;
    try{r?.abort()}catch{}
    this.prefix='';this.text='';this.onText('');this.change('ready');
  }
}
