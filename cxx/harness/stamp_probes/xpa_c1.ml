module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let ( |> ) x f = f x let y = S.empty |> S.cardinal
end
