(** Shapes *)
type t = Circle of float | Rect of float * float
val area : t -> float
val describe : t -> string
