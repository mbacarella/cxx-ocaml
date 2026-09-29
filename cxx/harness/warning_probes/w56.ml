type void = |
let f (x : (int, void) Either.t) = match x with Either.Left n -> n | Either.Right _ -> 0
