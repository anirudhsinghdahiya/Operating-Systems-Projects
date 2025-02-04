#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>  // For converting characters to lowercase

#define MAX_SIDES 10
#define MAX_LETTERS_PER_SIDE 10

// A simple linked list structure for storing the dictionary words
struct DictionaryNode {
    char *word;
    struct DictionaryNode *next;
};

// Function declarations
void free_dictionary(struct DictionaryNode *head);
void read_solution(struct DictionaryNode *dictionary, const int *letter_on_board, const int *letter_to_side);

// Convert a string to lowercase
void to_lowercase(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower(str[i]);  // Convert each character to lowercase
    }
}

// Function to check if all letters in the word are present on the board
// Also marks used letters
int check_and_track_letters(const char *word, int *letters_used, const int *letter_on_board) {
    for (int i = 0; word[i] != '\0'; i++) {
        char letter = word[i];

        // Check if the letter is valid (a-z)
        if (letter < 'a' || letter > 'z') {
            printf("Used a letter not present on the board\n");
            return 0;
        }

        int idx = letter - 'a';  // Convert letter to an index (0-25 for a-z)

        // Check if the letter exists on the board
        if (letter_on_board[idx]) {
            letters_used[idx] = 1;  // Mark the letter as used
        } else {
            printf("Used a letter not present on the board\n");
            return 0;
        }
    }
    return 1;
}

// Function to check if the first letter of the current word matches the last letter of the previous word
int check_word_chaining(const char *previous_word, const char *current_word) {
    if (previous_word[strlen(previous_word) - 1] != current_word[0]) {
        printf("First letter of word does not match last letter of previous word\n");
        return 0;
    }
    return 1;
}

// Function to read the board from a file
int read_board(const char *filename, char board[MAX_SIDES][MAX_LETTERS_PER_SIDE], int *num_sides) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error opening board file\n");
        return 1;
    }

    char line[MAX_LETTERS_PER_SIDE];
    *num_sides = 0;

    // Read the board line by line
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = '\0';  // Remove newline character
        to_lowercase(line);  // Convert the line to lowercase

        if (strlen(line) == 0) {
            continue;  // Skip empty lines
        }

        if (*num_sides >= MAX_SIDES) {
            fprintf(stderr, "Board has too many sides\n");
            fclose(file);
            return 1;
        }

        strcpy(board[*num_sides], line);  // Copy the side to the board array
        (*num_sides)++;
    }

    fclose(file);
    return 0;
}

// Function to read the dictionary from a file and store it in a linked list
struct DictionaryNode* read_dictionary(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening dictionary file");
        return NULL;
    }

    char word[100];
    struct DictionaryNode *head = NULL;
    struct DictionaryNode *tail = NULL;

    // Read each word from the file and add it to the linked list
    while (fgets(word, sizeof(word), file)) {
        word[strcspn(word, "\n")] = '\0';  // Remove newline
        to_lowercase(word);  // Convert the word to lowercase

        // Allocate memory for the new node
        struct DictionaryNode *new_node = malloc(sizeof(struct DictionaryNode));
        if (!new_node) {
            perror("Error allocating memory");
            return NULL;
        }

        // Allocate memory for the word and copy it
        new_node->word = malloc(strlen(word) + 1);  // Allocate memory for the word
        if (!new_node->word) {
            perror("Error allocating memory for word");
            free(new_node);  // Free the node if allocation fails
            return NULL;
        }
        strcpy(new_node->word, word);  // Copy the word into the node

        new_node->next = NULL;

        // Add the new node to the list
        if (head == NULL) {
            head = new_node;
        } else {
            tail->next = new_node;
        }
        tail = new_node;
    }

    fclose(file);
    return head;
}

// Function to free the linked list memory for the dictionary
void free_dictionary(struct DictionaryNode *head) {
    struct DictionaryNode *current = head;
    while (current != NULL) {
        struct DictionaryNode *next = current->next;
        free(current->word);  // Free the word string
        free(current);  // Free the node
        current = next;
    }
}

// Function to check if a word is in the dictionary (linked list search)
int word_in_dictionary(struct DictionaryNode *head, const char *word) {
    struct DictionaryNode *current = head;
    while (current != NULL) {
        if (strcmp(current->word, word) == 0) {
            return 1;  // Word found
        }
        current = current->next;
    }
    return 0;  // Word not found
}

// Function to read solution words from standard input and validate them
void read_solution(struct DictionaryNode *dictionary, const int *letter_on_board, const int *letter_to_side) {
    char word[100];
    int letters_used[26] = {0};  // Track used letters
    char previous_word[100] = "";  // Store the previous word for chaining

    // Read words from standard input
    while (fgets(word, sizeof(word), stdin)) {
        word[strcspn(word, "\n")] = '\0';  // Remove newline
        to_lowercase(word);  // Convert to lowercase

        // Check if the word is in the dictionary
        if (!word_in_dictionary(dictionary, word)) {
            printf("Word not found in dictionary\n");
            exit(0);
        }

        // Check if the word uses valid letters from the board and track used letters
        if (!check_and_track_letters(word, letters_used, letter_on_board)) {
            exit(0);  // Invalid word
        }

        // Check same-side letter usage
        for (int i = 0; word[i + 1] != '\0'; i++) {
            int idx_current = word[i] - 'a';
            int idx_next = word[i + 1] - 'a';

            int side_current = letter_to_side[idx_current];
            int side_next = letter_to_side[idx_next];

            if (side_current == side_next) {
                printf("Same-side letter used consecutively\n");
                exit(0);
            }
        }

        // Check word chaining (skip for the first word)
        if (previous_word[0] != '\0') {
            if (!check_word_chaining(previous_word, word)) {
                exit(0);  // Chaining rule violated
            }
        }

        // Update previous word for the next iteration
        strcpy(previous_word, word);

        // Check if all letters have been used
        int all_letters_used = 1;
        for (int i = 0; i < 26; i++) {
            if (letter_on_board[i] && !letters_used[i]) {
                all_letters_used = 0;
                break;
            }
        }

        if (all_letters_used) {
            printf("Correct\n");
            exit(0);
        }
    }

    printf("Not all letters used\n");
    exit(0);
}

// Main function: reads the board and dictionary, then processes the solution
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <board_file> <dictionary_file>\n", argv[0]);
        return 1;
    }

    // Read the board
    char board[MAX_SIDES][MAX_LETTERS_PER_SIDE];
    int num_sides;
    if (read_board(argv[1], board, &num_sides) != 0) {
        return 1;
    }

    // Validate the board and map letters to sides
    int letter_to_side[26];
    int letter_on_board[26] = {0};  // Initialize all to 0
    for (int i = 0; i < 26; i++) {
        letter_to_side[i] = -1;  // Initialize to -1
    }

    // Map the letters on the board to their sides
    for (int side = 0; side < num_sides; side++) {
        for (int j = 0; board[side][j] != '\0'; j++) {
            char letter = tolower(board[side][j]);

            if (letter < 'a' || letter > 'z') {
                printf("Invalid board\n");
                return 1;
            }

            int idx = letter - 'a';

            if (letter_to_side[idx] != -1) {
                printf("Invalid board\n");
                return 1;
            } else {
                letter_to_side[idx] = side;
                letter_on_board[idx] = 1;  // Mark letter as present
            }
        }
    }

    if (num_sides < 3) {
        printf("Invalid board\n");
        return 1;
    }

    // Read the dictionary
    struct DictionaryNode *dictionary = read_dictionary(argv[2]);
    if (dictionary == NULL) {
        return 1;
    }

    // Read and process the solution words from stdin
    read_solution(dictionary, letter_on_board, letter_to_side);

    // Free the dictionary memory
    free_dictionary(dictionary);

    return 0;
}
