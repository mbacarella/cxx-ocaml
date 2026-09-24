(* two parameters, and a second use costs nothing more *)
module type S = sig
  val h : (int, int) Hashtbl.t
  val g : (int, string) Hashtbl.t -> int
end
let z = 1
