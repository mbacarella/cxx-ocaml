module V = struct type t = Constr | Other end
let f ?(x : V.t = Constr) () = x
