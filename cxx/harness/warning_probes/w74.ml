type 'a mut = {mutable mut : 'a}
type t = {a : bool; b : (int, int) Either.t mut}
let f input = match input with
  | {a = false; b = _} -> Error 1
  | {a = _; b = {mut = Either.Right _}} -> Error 2
  | {a = _; b = _} when (input.b.mut <- Either.Right 3; false) -> Error 3
  | {a = true; b = {mut = Either.Left y}} -> Ok y
