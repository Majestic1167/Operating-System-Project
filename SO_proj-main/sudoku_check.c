#include "sudoku_check.h"

/* Check full solution */
int check_sudoku_solution(int grid[9][9]) {
    int row[9][10] = {0}, col[9][10] = {0}, box[9][10] = {0};
    
    for (int i = 0; i < 9; i++) {
        for (int j = 0; j < 9; j++) {
            int n = grid[i][j];
            
            /* Check if number is valid */
            if (n < 1 || n > 9) return 0;
            
            /* Calculate box index */
            int k = (i / 3) * 3 + (j / 3);
            
            /* Check for duplicates */
            if (row[i][n] || col[j][n] || box[k][n]) return 0;
            
            /* Mark as used */
            row[i][n] = col[j][n] = box[k][n] = 1;
        }
    }
    return 1;
}

/* Check partial solution (only duplicates matter) */
int check_sudoku_partial(int grid[9][9]) {
    int row[9][10] = {0}, col[9][10] = {0}, box[9][10] = {0};
    
    for (int i = 0; i < 9; i++) {
        for (int j = 0; j < 9; j++) {
            int n = grid[i][j];
            
            /* Skip empty cells */
            if (n == 0) continue;
            
            /* Check if number is valid */
            if (n < 1 || n > 9) return 0;
            
            /* Calculate box index */
            int k = (i / 3) * 3 + (j / 3);
            
            /* Check for duplicates */
            if (row[i][n] || col[j][n] || box[k][n]) return 0;
            
            /* Mark as used */
            row[i][n] = col[j][n] = box[k][n] = 1;
        }
    }
    return 1;
}

/* Check if grid is completely filled */
int is_complete(int grid[9][9]) {
    for (int i = 0; i < 9; i++) {
        for (int j = 0; j < 9; j++) {
            if (grid[i][j] == 0) return 0;
        }
    }
    return 1;
}