module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x = X.mk () } end
module M = F(struct type t = int let mk () = 0 end)
let f () = M.mk ()
