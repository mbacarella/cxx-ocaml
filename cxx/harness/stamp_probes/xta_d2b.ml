module TT = struct module N = struct let x = 1 let y = 2 type t = int end
  let z = 3 end
module T = TT
let f () = TT.N.x
