module TT = struct module N = struct module M = struct let a = 1 let b = 2 end
  let x = 1 end end
module T = TT
let f () = T.N.x + T.N.M.b
