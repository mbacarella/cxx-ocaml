module F (X : sig type t end) = struct let l (x : X.t) = [x] end
let f = let module M = F(struct type t = char end) in M.l
