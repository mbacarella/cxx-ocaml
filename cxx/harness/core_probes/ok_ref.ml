let r = ref 0
let () = r := !r + 1
let v = r.contents
let s = { contents = 3 }
let () = s.contents <- 4
let () = incr r
