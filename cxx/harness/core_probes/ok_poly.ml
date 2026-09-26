let id x = x
let p = (id 1, id "a")
let l = [id; (fun x -> x)]
let r = ref []
