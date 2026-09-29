(** Objects, classes, polymorphic variants *)
class counter : int -> object ('a) val mutable n : int method get : int method incr : 'a end
class virtual shape : string -> object method virtual area : float method name : string end
class square : float -> object method area : float method name : string end
val total : #shape list -> float
val o : < hello : string; twice : int -> int >
type color = [ `Green | `Red | `Rgb of int * int * int ]
val to_int : [< color ] -> int
exception Found of int
val find : (int -> bool) -> int list -> int option
val r : int ref
val fmt : string
val lazy_v : int Lazy.t
val fib : int -> int
val arr : int array
