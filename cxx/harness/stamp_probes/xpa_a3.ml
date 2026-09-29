module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let y = S.empty |> S.cardinal |> string_of_int
end
