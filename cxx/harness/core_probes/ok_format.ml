let s = Printf.sprintf "%d %s %c %f" 1 "a" 'c' 2.0
let f = Printf.sprintf "%a" (fun () x -> x) "q"
let g = Format.asprintf "%i@." 3
let fmt = format_of_string "%d"
