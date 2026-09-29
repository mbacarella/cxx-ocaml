(* an arrow domain tuple spans its tokens, parentheses included *)
type v
type i
type c
module type S = sig
  val f : c * (v * i) -> (c * i) option
  val g : (v * i) * c -> c
  val k : lbl:c * (v * i) -> c
end
