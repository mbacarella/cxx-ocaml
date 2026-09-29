(* Doc comments through a rewriter that copies every location record: the
   doc attribute's expression and structure item get equal-valued but
   distinct locations, which the compiler must keep distinct (-g summaries,
   .cmt). *)
module type S = sig
  (** [f x y] is documented. *)
  val f : int -> bool (** a flag *) -> int

  (** [g] too *)
  val g : string
end

(** a documented function *)
let h (module M : S) x = M.f x true + String.length M.g
