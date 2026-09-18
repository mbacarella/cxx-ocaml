module TT = struct module N = struct let x = 1 let y = 2 end module P = struct
  let y = 2 end end
let f () = let module T = TT in T.N.x + T.P.y
