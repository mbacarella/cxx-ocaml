module type S = sig type t end
module P : S

(** Doc before the module substitution. *)
module M := P [@@deprecated "use P"] [@@foo]
(** Doc after it. *)

(** Doc before the module type substitution. *)
module type T := S [@@bar]

module type [@b1] U := S [@@b2]
(** trailing *)

val x : int
