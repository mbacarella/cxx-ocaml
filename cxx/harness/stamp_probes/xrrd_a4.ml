(* a local three-type group re-exported *)
module M = struct
  type t = A of u and u = B of v | C and v = t list
end
include M
