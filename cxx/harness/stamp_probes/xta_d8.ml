module TT = struct module N = struct let x = 1 let y = 2 type t = int end
  let z = 3 end
let g () = TT.N.y
let f () = let module T = TT in T.N.x
