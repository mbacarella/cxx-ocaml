module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = if true then S.cardinal else raise Exit
end
