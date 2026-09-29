(* A PATHLESS argument is rebuilt for nothing. *)
module Mk (X : sig type t end) :
  sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
module X : sig type t end = struct type t = int end
module Y : sig type t end = struct type t = bool end
module type Sml = sig type k type 'a t val a : 'a t end
module S : Sml = Mk (struct type t = int end)
