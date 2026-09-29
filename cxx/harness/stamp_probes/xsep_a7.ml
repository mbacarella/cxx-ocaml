(* a `with type` re-derives the refined declaration: Sep *)
module type T = sig type 'a w end
module type U = T with type 'a w = 'a
