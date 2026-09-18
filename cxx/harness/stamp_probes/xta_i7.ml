module TT = struct module N = struct let x = 1 let y = 2 end module P = struct
  let y = 2 end end
module T = TT
module U = TT
let f () = T.N.x + U.P.y
