(* Two unascribed and one ascribed. *)
module Mk (X : sig type t end) :
  sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
module X : sig type t end = struct type t = int end
module Y : sig type t end = struct type t = bool end
module type Sml = sig type k type 'a t val a : 'a t end
module S = Mk (X)
module T = Mk (Y)
module U : Sml = Mk (X)
