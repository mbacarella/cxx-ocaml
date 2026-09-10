(* companion for s637: the alias is WRITTEN, not strengthened. *)
module M = struct
  type t = string
  module Set = Set.Make (String)
end
type t = string
module Set = M.Set
