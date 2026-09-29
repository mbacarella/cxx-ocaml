module TT = struct module N = struct module M = struct let a = 1 let b = 2 end
  let x = 1 end end
let f () = let module T = TT.N in T.M.a
