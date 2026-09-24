(* a signature `val` keeps its attribute *)
module type S = sig
  val f : int -> int [@@inline]
  external p : int -> int = "p" [@@noalloc]
end
