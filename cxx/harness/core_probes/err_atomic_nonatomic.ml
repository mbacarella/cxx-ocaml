let r = ref 0
let bad = [%atomic.loc r.contents]
