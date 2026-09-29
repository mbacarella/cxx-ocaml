let apply f x = f x
let pipe x f = f x
let a = 3 |> string_of_int
let b = string_of_int @@ 4
let c = List.iter print_int
