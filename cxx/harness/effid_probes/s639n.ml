(* companion for s639: the same include-strengthened submodule alias as s636n. *)
module M = struct
  type t = string
  module Set = Set.Make (String)
end
include M
