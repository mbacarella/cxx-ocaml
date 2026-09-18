module TT = struct module N = struct class c = object end exception E
  module type S = sig end module Q = struct end let x = 1 end end
let f () = let module T = TT in T.N.x
