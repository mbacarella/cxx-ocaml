module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x = X.mk () } end
module B = struct type t = float let mk () = 0. end
module P : sig val f : unit -> unit end = struct
  module M = F(B)
  let f () = ignore (M.mk ())
end
