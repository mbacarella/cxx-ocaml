exception E of exn * exn
let f x = match x with E ((Not_found | _), (Exit | _)) -> 1
