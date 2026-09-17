module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x = X.mk () } end
module H (Y : T) = struct module M = F(Y) let f () = M.mk () end
