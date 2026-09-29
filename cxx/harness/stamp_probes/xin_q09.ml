module String_id : sig type t = private string val of_string : string -> t
end = struct type t = string let of_string s = s end
let () = let foo = String_id.of_string "foo" in print_string (foo :> string)
