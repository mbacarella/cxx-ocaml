module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let y = S.empty |> (if true then S.cardinal else fun _ -> 1)
end
