(** The interface *)
type 'a tree = Leaf | Node of 'a tree * 'a * 'a tree
type point = { x : float; mutable y : float }
type _ expr = Int : int -> int expr | Add : int expr * int expr -> int expr
exception Stop of string
val size : 'a tree -> int
val eval : 'a expr -> 'a
module type S = sig type t val v : t end
module F (X : S) : S with type t = X.t list
module I : S with type t = int list
class counter : int -> object ('s) val mutable n : int method incr : 's method get : int end
val poly : [< `A | `B ] -> int
val labels : a:int -> ?b:int -> unit -> int
val lets : int option
val objs : int
val pack : (module S with type t = int list)
val local : int
val arrays : int
val tuple : int * string * char * float * int32 * int64 * nativeint
val f : 'a -> 'a
val lazyv : int lazy_t
val recd : point
val seq : unit
